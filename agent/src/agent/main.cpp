#include "common/channel.hpp"
#include "common/config.hpp"
#include "common/crypto.hpp"
#include "common/log.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <csignal>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr const char* kVersion = "0.1.0";

enum class Command {
    Join,
    ShowConfig,
    Help,
    Version,
};

struct Config {
    std::string controller_host;
    uint16_t controller_port = 0;
    std::string id = "agent-01";
    std::string secret = "dev-secret";
    std::set<std::string> allowlist;
    wops::TlsOptions tls;
};

struct TargetStream {
    uint32_t id{};
    wops::socket_t target = wops::invalid_socket;
    std::atomic<bool> closed{false};
};

std::string trim_copy(const std::string& value) {
    size_t start = 0;
    while (start < value.size() && std::isspace(static_cast<unsigned char>(value[start]))) {
        ++start;
    }
    size_t end = value.size();
    while (end > start && std::isspace(static_cast<unsigned char>(value[end - 1]))) {
        --end;
    }
    return value.substr(start, end - start);
}

std::string make_hello_payload(const std::string& id) {
    return id;
}

bool parse_open_payload(const std::string& payload, std::string* host, uint16_t* port) {
    const auto sep = payload.find('\n');
    if (sep == std::string::npos) {
        return false;
    }
    *host = payload.substr(0, sep);
    int parsed = 0;
    try {
        parsed = std::stoi(payload.substr(sep + 1));
    } catch (...) {
        return false;
    }
    if (host->empty() || parsed <= 0 || parsed > 65535) {
        return false;
    }
    *port = static_cast<uint16_t>(parsed);
    return true;
}

class Agent {
   public:
    explicit Agent(Config config) : config_(std::move(config)) {}

    void run();
    void request_stop();

   private:
    bool connect_once();
    void session_loop();
    void heartbeat_loop();
    bool send(const wops::Frame& frame);

    void handle_open(const wops::Frame& frame);
    void handle_data(const wops::Frame& frame);
    void handle_close(uint32_t stream_id, bool notify_controller);
    void target_reader(std::shared_ptr<TargetStream> stream);
    void close_all_streams();
    std::shared_ptr<TargetStream> find_stream(uint32_t stream_id);
    void add_stream(const std::shared_ptr<TargetStream>& stream);
    void remove_stream(uint32_t stream_id);

    Config config_;
    std::unique_ptr<wops::Channel> channel_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> connected_{false};
    std::mutex send_mu_;
    std::mutex streams_mu_;
    std::map<uint32_t, std::shared_ptr<TargetStream>> streams_;
};

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested.store(true);
}

void install_signal_handlers() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

bool Agent::connect_once() {
    std::string error;
    auto sock = wops::connect_tcp(config_.controller_host, config_.controller_port, &error);
    if (!wops::socket_valid(sock)) {
        wops::log_warn("agent", "controller_connect_failed", {{"error", error}});
        return false;
    }

    channel_ = wops::Channel::client(sock, config_.controller_host, config_.tls, &error);
    if (!channel_) {
        wops::log_warn("agent", "controller_tls_failed", {{"error", error}});
        return false;
    }

    if (!send({wops::FrameType::Hello, wops::kControlStream, wops::text_payload(make_hello_payload(config_.id))})) {
        channel_->close();
        channel_.reset();
        return false;
    }

    auto challenge = channel_->recv_frame(&error);
    if (!challenge || challenge->type != wops::FrameType::AuthChallenge) {
        wops::log_error("agent", "controller_auth_challenge_missing");
        channel_->close();
        channel_.reset();
        return false;
    }

    const auto nonce = wops::payload_text(challenge->payload);
    const auto proof = wops::hmac_sha256_hex(config_.secret, config_.id + "\n" + nonce);
    if (!send({wops::FrameType::AuthResponse, wops::kControlStream, wops::text_payload(proof)})) {
        channel_->close();
        channel_.reset();
        return false;
    }

    auto reply = channel_->recv_frame(&error);
    if (!reply || reply->type != wops::FrameType::HelloOk) {
        wops::log_error("agent", "controller_rejected_hello");
        channel_->close();
        channel_.reset();
        return false;
    }

    connected_.store(true);
    wops::log_info("agent", "controller_connected", {{"agent", config_.id}});
    return true;
}

void Agent::run() {
    while (!stopping_.load()) {
        if (!connect_once()) {
            std::this_thread::sleep_for(2s);
            continue;
        }

        std::thread([this] { heartbeat_loop(); }).detach();
        session_loop();
        connected_.store(false);
        close_all_streams();
        if (channel_) {
            channel_->shutdown();
            channel_->close();
            channel_.reset();
        }
        if (stopping_.load()) {
            break;
        }
        wops::log_warn("agent", "controller_disconnected_reconnecting");
        std::this_thread::sleep_for(2s);
    }
}

void Agent::request_stop() {
    const bool was_stopping = stopping_.exchange(true);
    if (was_stopping) {
        return;
    }
    connected_.store(false);
    if (channel_) {
        channel_->shutdown();
        channel_->close();
    }
    close_all_streams();
}

void Agent::session_loop() {
    while (connected_.load()) {
        std::string error;
        if (!channel_) {
            break;
        }
        auto frame = channel_->recv_frame(&error);
        if (!frame) {
            break;
        }

        switch (frame->type) {
            case wops::FrameType::Open:
                std::thread([this, frame = *frame] { handle_open(frame); }).detach();
                break;
            case wops::FrameType::Data:
                handle_data(*frame);
                break;
            case wops::FrameType::Close:
                handle_close(frame->stream_id, true);
                break;
            default:
                send({wops::FrameType::Error, frame->stream_id, wops::text_payload("unsupported frame: " + wops::frame_type_name(frame->type))});
                break;
        }
    }
}

void Agent::heartbeat_loop() {
    while (connected_.load()) {
        std::this_thread::sleep_for(5s);
        if (!connected_.load()) {
            break;
        }
        if (!send({wops::FrameType::Heartbeat, wops::kControlStream, wops::text_payload("alive")})) {
            connected_.store(false);
            break;
        }
    }
}

bool Agent::send(const wops::Frame& frame) {
    std::string error;
    std::lock_guard<std::mutex> lock(send_mu_);
    if (!channel_ || !channel_->valid()) {
        return false;
    }
    if (!channel_->send_frame(frame, &error)) {
        wops::log_error("agent", "controller_send_failed", {{"error", error}});
        connected_.store(false);
        return false;
    }
    return true;
}

void Agent::handle_open(const wops::Frame& frame) {
    std::string host;
    uint16_t port = 0;
    if (!parse_open_payload(wops::payload_text(frame.payload), &host, &port)) {
        send({wops::FrameType::Error, frame.stream_id, wops::text_payload("invalid OPEN payload")});
        return;
    }

    const std::string endpoint = wops::endpoint_to_string(host, port);
    if (config_.allowlist.find(endpoint) == config_.allowlist.end()) {
        wops::log_warn("agent", "target_denied", {{"stream", std::to_string(frame.stream_id)}, {"target", endpoint}});
        send({wops::FrameType::Error, frame.stream_id, wops::text_payload("target not allowed: " + endpoint)});
        return;
    }

    std::string error;
    auto target = wops::connect_tcp(host, port, &error);
    if (!wops::socket_valid(target)) {
        send({wops::FrameType::Error, frame.stream_id, wops::text_payload("target connect failed: " + error)});
        return;
    }

    auto stream = std::make_shared<TargetStream>();
    stream->id = frame.stream_id;
    stream->target = target;
    add_stream(stream);

    wops::log_info("agent", "stream_opened", {{"stream", std::to_string(frame.stream_id)}, {"target", endpoint}});
    send({wops::FrameType::OpenOk, frame.stream_id, wops::text_payload("ok")});
    std::thread([this, stream] { target_reader(stream); }).detach();
}

void Agent::handle_data(const wops::Frame& frame) {
    auto stream = find_stream(frame.stream_id);
    if (!stream || stream->closed.load()) {
        send({wops::FrameType::Error, frame.stream_id, wops::text_payload("unknown stream")});
        return;
    }

    std::string error;
    if (!wops::send_all(stream->target, frame.payload.data(), frame.payload.size(), &error)) {
        handle_close(frame.stream_id, true);
    }
}

void Agent::handle_close(uint32_t stream_id, bool notify_controller) {
    auto stream = find_stream(stream_id);
    if (!stream) {
        return;
    }

    if (!stream->closed.exchange(true)) {
        wops::shutdown_socket(stream->target);
        wops::close_socket(stream->target);
        remove_stream(stream_id);
        if (notify_controller) {
            send({wops::FrameType::Close, stream_id, {}});
        }
        wops::log_info("agent", "stream_closed", {{"stream", std::to_string(stream_id)}});
    }
}

void Agent::target_reader(std::shared_ptr<TargetStream> stream) {
    std::vector<uint8_t> buffer(32 * 1024);
    while (!stream->closed.load() && connected_.load()) {
        std::string error;
        const int got = wops::recv_some(stream->target, buffer.data(), buffer.size(), &error);
        if (got <= 0) {
            break;
        }
        std::vector<uint8_t> data(buffer.begin(), buffer.begin() + got);
        if (!send({wops::FrameType::Data, stream->id, std::move(data)})) {
            break;
        }
    }

    handle_close(stream->id, true);
}

void Agent::close_all_streams() {
    std::map<uint32_t, std::shared_ptr<TargetStream>> copy;
    {
        std::lock_guard<std::mutex> lock(streams_mu_);
        copy = streams_;
        streams_.clear();
    }
    for (auto& [_, stream] : copy) {
        if (!stream->closed.exchange(true)) {
            wops::shutdown_socket(stream->target);
            wops::close_socket(stream->target);
        }
    }
}

std::shared_ptr<TargetStream> Agent::find_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mu_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return {};
    }
    return it->second;
}

void Agent::add_stream(const std::shared_ptr<TargetStream>& stream) {
    std::lock_guard<std::mutex> lock(streams_mu_);
    streams_[stream->id] = stream;
}

void Agent::remove_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mu_);
    streams_.erase(stream_id);
}

void print_usage() {
    std::cout << "usage:\n";
    std::cout << "  webhookops-agent join [options]\n";
    std::cout << "  webhookops-agent show-config [options]\n";
    std::cout << "  webhookops-agent version\n\n";
    std::cout << "options:\n";
    std::cout << "  --config path\n";
    std::cout << "  --tunnel host:port\n";
    std::cout << "  --id agent-id\n";
    std::cout << "  --secret secret\n";
    std::cout << "  --token token    legacy alias for --secret\n";
    std::cout << "  --allow host:port\n";
    std::cout << "  --tls\n";
    std::cout << "  --tls-ca path\n";
    std::cout << "  --tls-insecure\n";
}

Command detect_command(int argc, char** argv, int* first_option) {
    *first_option = 1;
    if (argc < 2) {
        return Command::Join;
    }

    const std::string arg = argv[1];
    if (arg == "join") {
        *first_option = 2;
        return Command::Join;
    }
    if (arg == "show-config") {
        *first_option = 2;
        return Command::ShowConfig;
    }
    if (arg == "version" || arg == "--version") {
        *first_option = 2;
        return Command::Version;
    }
    if (arg == "help" || arg == "--help" || arg == "-h") {
        *first_option = 2;
        return Command::Help;
    }
    return Command::Join;
}

void print_resolved_config(const Config& config) {
    std::cout << "tunnel=" << wops::endpoint_to_string(config.controller_host, config.controller_port) << "\n";
    std::cout << "id=" << config.id << "\n";
    std::cout << "secret=(set)\n";
    std::cout << "tls=" << (config.tls.enabled ? "true" : "false") << "\n";
    if (!config.tls.ca_path.empty()) {
        std::cout << "tls_ca=" << config.tls.ca_path << "\n";
    }
    if (config.tls.insecure_skip_verify) {
        std::cout << "tls_insecure=true\n";
    }
    for (const auto& endpoint : config.allowlist) {
        std::cout << "allow=" << endpoint << "\n";
    }
}

bool apply_node_config(const wops::ConfigFile& file, Config* config) {
    if (const auto value = wops::config_get(file, "tunnel"); !value.empty()) {
        const auto endpoint = wops::parse_host_port(value);
        if (!endpoint) {
            std::cerr << "invalid config tunnel endpoint\n";
            return false;
        }
        config->controller_host = endpoint->first;
        config->controller_port = endpoint->second;
    }

    if (const auto value = wops::config_get(file, "id"); !value.empty()) {
        config->id = value;
    }

    if (const auto value = wops::config_get(file, "secret"); !value.empty()) {
        config->secret = value;
    } else if (const auto legacy = wops::config_get(file, "token"); !legacy.empty()) {
        config->secret = legacy;
    }

    for (const auto& value : wops::config_get_all(file, "allow")) {
        const auto endpoint = wops::parse_host_port(value);
        if (!endpoint) {
            std::cerr << "invalid config allow endpoint: " << value << "\n";
            return false;
        }
        config->allowlist.insert(wops::endpoint_to_string(endpoint->first, endpoint->second));
    }

    if (const auto value = wops::config_get(file, "tls"); value == "true" || value == "1" || value == "yes") {
        config->tls.enabled = true;
    }

    if (const auto value = wops::config_get(file, "tls_ca"); !value.empty()) {
        config->tls.ca_path = value;
        config->tls.enabled = true;
    }

    if (const auto value = wops::config_get(file, "tls_insecure"); value == "true" || value == "1" || value == "yes") {
        config->tls.insecure_skip_verify = true;
        config->tls.enabled = true;
    }

    return true;
}

bool load_config_arg(int argc, char** argv, int first_option, Config* config) {
    for (int i = first_option; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--config") {
            if (i + 1 >= argc) {
                std::cerr << "missing value for --config\n";
                return false;
            }
            wops::ConfigFile file;
            std::string error;
            if (!wops::load_config_file(argv[i + 1], &file, &error)) {
                std::cerr << error << "\n";
                return false;
            }
            return apply_node_config(file, config);
        }
    }
    return true;
}

bool parse_args(int argc, char** argv, Config* config) {
    int first_option = 1;
    (void)detect_command(argc, argv, &first_option);
    if (!load_config_arg(argc, argv, first_option, config)) {
        return false;
    }

    for (int i = first_option; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](const std::string& name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "missing value for " << name << "\n";
                return {};
            }
            return argv[++i];
        };

        if (arg == "--help" || arg == "-h") {
            print_usage();
            return false;
        }
        if (arg == "--config") {
            (void)require_value(arg);
        } else if (arg == "--tunnel") {
            const auto endpoint = wops::parse_host_port(require_value(arg));
            if (!endpoint) {
                std::cerr << "invalid --tunnel endpoint\n";
                return false;
            }
            config->controller_host = endpoint->first;
            config->controller_port = endpoint->second;
        } else if (arg == "--id") {
            config->id = require_value(arg);
            if (config->id.empty()) {
                std::cerr << "--id cannot be empty\n";
                return false;
            }
        } else if (arg == "--secret" || arg == "--token") {
            config->secret = require_value(arg);
            if (config->secret.empty()) {
                std::cerr << arg << " cannot be empty\n";
                return false;
            }
        } else if (arg == "--allow") {
            const auto endpoint = wops::parse_host_port(require_value(arg));
            if (!endpoint) {
                std::cerr << "invalid --allow endpoint\n";
                return false;
            }
            config->allowlist.insert(wops::endpoint_to_string(endpoint->first, endpoint->second));
        } else if (arg == "--tls") {
            config->tls.enabled = true;
        } else if (arg == "--tls-ca") {
            config->tls.ca_path = require_value(arg);
            config->tls.enabled = true;
        } else if (arg == "--tls-insecure") {
            config->tls.insecure_skip_verify = true;
            config->tls.enabled = true;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return false;
        }
    }

    if (config->controller_host.empty() || config->controller_port == 0) {
        std::cerr << "--tunnel is required\n";
        return false;
    }

    return true;
}

}  // namespace

int main(int argc, char** argv) {
    install_signal_handlers();

    int first_option = 1;
    const Command command = detect_command(argc, argv, &first_option);
    if (command == Command::Help) {
        print_usage();
        return 0;
    }
    if (command == Command::Version) {
        std::cout << "webhookops-agent " << kVersion << "\n";
        return 0;
    }

    Config config;
    if (!parse_args(argc, argv, &config)) {
        return 1;
    }

    if (command == Command::ShowConfig) {
        print_resolved_config(config);
        return 0;
    }

    if (config.allowlist.empty()) {
        wops::log_warn("agent", "allowlist_empty");
    } else {
        wops::log_info("agent", "allowlist_loaded", {{"targets", std::to_string(config.allowlist.size())}});
        for (const auto& endpoint : config.allowlist) {
            wops::log_info("agent", "allow_target", {{"target", endpoint}});
        }
    }

    std::string error;
    if (!wops::net_init(&error)) {
        std::cerr << error << "\n";
        return 1;
    }

    Agent agent(config);
    std::thread signal_watcher([&agent] {
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(100ms);
        }
        agent.request_stop();
    });
    agent.run();
    agent.request_stop();
    g_stop_requested.store(true);
    if (signal_watcher.joinable()) {
        signal_watcher.join();
    }
    wops::net_cleanup();
    return 0;
}
