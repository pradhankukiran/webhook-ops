#include "common/channel.hpp"
#include "common/config.hpp"
#include "common/crypto.hpp"
#include "common/log.hpp"
#include "common/net.hpp"
#include "common/protocol.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cctype>
#include <cstdint>
#include <csignal>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

using namespace std::chrono_literals;

namespace {

constexpr const char* kVersion = "0.1.0";

enum class Command {
    Run,
    ShowConfig,
    Help,
    Version,
};

struct Config {
    std::string control_host = "0.0.0.0";
    uint16_t control_port = 9700;
    std::string proxy_host = "127.0.0.1";
    uint16_t proxy_port = 9080;
    std::string secret = "dev-secret";
    std::string state_path = "webhookops-tunnel.state";
    std::string initial_selected_agent;
    std::string auth_url;
    std::string auth_token;
    std::string status_url;
    std::string status_token;
    wops::TlsOptions tls;
};

struct ProxyStream {
    uint32_t id{};
    wops::socket_t client = wops::invalid_socket;
    std::atomic<bool> client_closed{false};
    std::mutex mu;
    std::condition_variable cv;
    bool opened = false;
    bool closed = false;
    std::string error;
};

class Controller;

class AgentSession : public std::enable_shared_from_this<AgentSession> {
   public:
    AgentSession(Controller& owner, std::unique_ptr<wops::Channel> channel, std::string id)
        : owner_(owner), channel_(std::move(channel)), id_(std::move(id)) {}

    const std::string& id() const { return id_; }
    bool alive() const { return alive_.load(); }

    void start();
    bool send(const wops::Frame& frame);
    void register_stream(const std::shared_ptr<ProxyStream>& stream);
    void unregister_stream(uint32_t stream_id);
    void close_all_streams(const std::string& reason);
    void close();

   private:
    void read_loop();
    std::shared_ptr<ProxyStream> find_stream(uint32_t stream_id);

    Controller& owner_;
    std::unique_ptr<wops::Channel> channel_;
    std::string id_;
    std::atomic<bool> alive_{true};
    std::mutex send_mu_;
    std::mutex streams_mu_;
    std::map<uint32_t, std::shared_ptr<ProxyStream>> streams_;
};

std::string lower_copy(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

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

std::vector<std::string> split_lines(const std::string& value) {
    std::vector<std::string> lines;
    size_t pos = 0;
    while (pos <= value.size()) {
        size_t next = value.find("\r\n", pos);
        if (next == std::string::npos) {
            if (pos < value.size()) {
                lines.push_back(value.substr(pos));
            }
            break;
        }
        lines.push_back(value.substr(pos, next - pos));
        pos = next + 2;
    }
    return lines;
}

bool starts_with_ci(const std::string& value, const std::string& prefix) {
    if (value.size() < prefix.size()) {
        return false;
    }
    return lower_copy(value.substr(0, prefix.size())) == lower_copy(prefix);
}

bool read_http_header(wops::socket_t client, std::string* header, std::vector<uint8_t>* leftover, std::string* error) {
    std::vector<uint8_t> buffer;
    buffer.reserve(8192);
    std::vector<uint8_t> scratch(4096);
    const std::string marker = "\r\n\r\n";

    while (buffer.size() < 64 * 1024) {
        const int got = wops::recv_some(client, scratch.data(), scratch.size(), error);
        if (got <= 0) {
            return false;
        }
        buffer.insert(buffer.end(), scratch.begin(), scratch.begin() + got);

        auto it = std::search(buffer.begin(), buffer.end(), marker.begin(), marker.end());
        if (it != buffer.end()) {
            const size_t header_len = static_cast<size_t>((it - buffer.begin()) + marker.size());
            *header = std::string(buffer.begin(), buffer.begin() + static_cast<long>(header_len));
            leftover->assign(buffer.begin() + static_cast<long>(header_len), buffer.end());
            return true;
        }
    }

    if (error) {
        *error = "HTTP header exceeded 64 KiB";
    }
    return false;
}

struct ParsedProxyRequest {
    bool ok = false;
    bool is_connect = false;
    std::string host;
    uint16_t port = 0;
    std::vector<uint8_t> initial_data;
    std::string error;
};

std::string find_host_header(const std::vector<std::string>& lines) {
    for (const auto& line : lines) {
        if (starts_with_ci(line, "host:")) {
            return trim_copy(line.substr(5));
        }
    }
    return {};
}

ParsedProxyRequest parse_proxy_request(const std::string& raw_header, const std::vector<uint8_t>& leftover) {
    ParsedProxyRequest parsed;
    const auto lines = split_lines(raw_header.substr(0, raw_header.size() >= 4 ? raw_header.size() - 4 : raw_header.size()));
    if (lines.empty()) {
        parsed.error = "empty request";
        return parsed;
    }

    std::istringstream first_line(lines[0]);
    std::string method;
    std::string target;
    std::string version;
    first_line >> method >> target >> version;
    if (method.empty() || target.empty() || version.empty()) {
        parsed.error = "invalid request line";
        return parsed;
    }

    if (lower_copy(method) == "connect") {
        const auto endpoint = wops::parse_host_port(target, 443);
        if (!endpoint) {
            parsed.error = "invalid CONNECT target";
            return parsed;
        }
        parsed.ok = true;
        parsed.is_connect = true;
        parsed.host = endpoint->first;
        parsed.port = endpoint->second;
        return parsed;
    }

    std::string path = target;
    bool absolute_http = false;
    if (starts_with_ci(target, "http://")) {
        absolute_http = true;
        const std::string rest = target.substr(7);
        const size_t slash = rest.find('/');
        const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
        path = slash == std::string::npos ? "/" : rest.substr(slash);
        const auto endpoint = wops::parse_host_port(host_port, 80);
        if (!endpoint) {
            parsed.error = "invalid http URL";
            return parsed;
        }
        parsed.host = endpoint->first;
        parsed.port = endpoint->second;
    } else if (starts_with_ci(target, "https://")) {
        parsed.error = "HTTPS proxying must use CONNECT";
        return parsed;
    } else {
        const auto host_header = find_host_header(lines);
        const auto endpoint = wops::parse_host_port(host_header, 80);
        if (!endpoint) {
            parsed.error = "missing or invalid Host header";
            return parsed;
        }
        parsed.host = endpoint->first;
        parsed.port = endpoint->second;
    }

    std::ostringstream rewritten;
    rewritten << method << " " << path << " " << version << "\r\n";

    bool saw_host = false;
    for (size_t i = 1; i < lines.size(); ++i) {
        if (lines[i].empty()) {
            continue;
        }
        if (starts_with_ci(lines[i], "proxy-connection:")) {
            continue;
        }
        if (starts_with_ci(lines[i], "host:")) {
            saw_host = true;
            if (absolute_http) {
                rewritten << "Host: " << wops::endpoint_to_string(parsed.host, parsed.port) << "\r\n";
            } else {
                rewritten << lines[i] << "\r\n";
            }
            continue;
        }
        rewritten << lines[i] << "\r\n";
    }
    if (!saw_host) {
        rewritten << "Host: " << wops::endpoint_to_string(parsed.host, parsed.port) << "\r\n";
    }
    rewritten << "\r\n";

    const auto header_text = rewritten.str();
    parsed.initial_data.assign(header_text.begin(), header_text.end());
    parsed.initial_data.insert(parsed.initial_data.end(), leftover.begin(), leftover.end());
    parsed.ok = true;
    return parsed;
}

void send_http_error(wops::socket_t client, int code, const std::string& reason) {
    std::ostringstream body;
    body << code << " " << reason << "\n";
    const std::string body_text = body.str();
    std::ostringstream response;
    response << "HTTP/1.1 " << code << " " << reason << "\r\n";
    response << "Connection: close\r\n";
    response << "Content-Type: text/plain\r\n";
    response << "Content-Length: " << body_text.size() << "\r\n\r\n";
    response << body_text;
    const auto text = response.str();
    std::string ignored;
    wops::send_all(client, reinterpret_cast<const uint8_t*>(text.data()), text.size(), &ignored);
}

void close_proxy_client(const std::shared_ptr<ProxyStream>& stream) {
    if (!stream->client_closed.exchange(true)) {
        wops::shutdown_socket(stream->client);
        wops::close_socket(stream->client);
    }
}

std::string make_open_payload(const std::string& host, uint16_t port) {
    return host + "\n" + std::to_string(port);
}

bool parse_hello(const std::string& payload, std::string* id) {
    *id = trim_copy(payload);
    return !id->empty();
}

struct ParsedHttpUrl {
    std::string host;
    uint16_t port = 80;
    std::string path = "/";
};

bool parse_http_url(const std::string& url, ParsedHttpUrl* parsed) {
    constexpr const char* prefix = "http://";
    if (!starts_with_ci(url, prefix)) {
        return false;
    }
    const std::string rest = url.substr(std::string(prefix).size());
    const size_t slash = rest.find('/');
    const std::string host_port = slash == std::string::npos ? rest : rest.substr(0, slash);
    const auto endpoint = wops::parse_host_port(host_port, 80);
    if (!endpoint) {
        return false;
    }
    parsed->host = endpoint->first;
    parsed->port = endpoint->second;
    parsed->path = slash == std::string::npos ? "/" : rest.substr(slash);
    return !parsed->host.empty();
}

std::string json_escape(const std::string& value) {
    std::ostringstream escaped;
    for (char ch : value) {
        switch (ch) {
            case '"':
                escaped << "\\\"";
                break;
            case '\\':
                escaped << "\\\\";
                break;
            case '\n':
                escaped << "\\n";
                break;
            case '\r':
                escaped << "\\r";
                break;
            case '\t':
                escaped << "\\t";
                break;
            default:
                const auto value = static_cast<unsigned char>(ch);
                if (value < 0x20) {
                    escaped << "\\u00";
                    const char* hex = "0123456789abcdef";
                    escaped << hex[(value >> 4) & 0x0f] << hex[value & 0x0f];
                } else {
                    escaped << ch;
                }
                break;
        }
    }
    return escaped.str();
}

std::string make_status_payload(
    const std::string& agent_id,
    const std::string& event,
    const std::map<std::string, std::string>& metadata) {
    std::ostringstream payload;
    payload << "{\"agent_id\":\"" << json_escape(agent_id) << "\",";
    payload << "\"event\":\"" << json_escape(event) << "\",";
    payload << "\"metadata\":{";
    bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) {
            payload << ",";
        }
        first = false;
        payload << "\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
    }
    payload << "}}";
    return payload.str();
}

bool post_agent_status(
    const Config& config,
    const std::string& agent_id,
    const std::string& event,
    const std::map<std::string, std::string>& metadata) {
    if (config.status_url.empty() || config.status_token.empty()) {
        return true;
    }

    ParsedHttpUrl url;
    if (!parse_http_url(config.status_url, &url)) {
        wops::log_warn("tunnel", "status_url_invalid", {{"url", config.status_url}});
        return false;
    }

    std::string error;
    auto sock = wops::connect_tcp(url.host, url.port, &error);
    if (!wops::socket_valid(sock)) {
        wops::log_warn("tunnel", "status_connect_failed", {{"error", error}});
        return false;
    }

    const std::string body = make_status_payload(agent_id, event, metadata);
    std::ostringstream request;
    request << "POST " << url.path << " HTTP/1.1\r\n";
    request << "Host: " << wops::endpoint_to_string(url.host, url.port) << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << body.size() << "\r\n";
    request << "X-WebhookOps-Tunnel-Token: " << config.status_token << "\r\n";
    request << "Connection: close\r\n\r\n";
    request << body;

    const std::string text = request.str();
    const bool sent = wops::send_all(sock, reinterpret_cast<const uint8_t*>(text.data()), text.size(), &error);
    if (!sent) {
        wops::log_warn("tunnel", "status_send_failed", {{"error", error}});
    }
    wops::close_socket(sock);
    return sent;
}

int post_json(
    const std::string& url_text,
    const std::string& token,
    const std::string& body,
    const std::string& log_context) {
    ParsedHttpUrl url;
    if (!parse_http_url(url_text, &url)) {
        wops::log_warn("tunnel", log_context + "_url_invalid", {{"url", url_text}});
        return 0;
    }

    std::string error;
    auto sock = wops::connect_tcp(url.host, url.port, &error);
    if (!wops::socket_valid(sock)) {
        wops::log_warn("tunnel", log_context + "_connect_failed", {{"error", error}});
        return 0;
    }

    std::ostringstream request;
    request << "POST " << url.path << " HTTP/1.1\r\n";
    request << "Host: " << wops::endpoint_to_string(url.host, url.port) << "\r\n";
    request << "Content-Type: application/json\r\n";
    request << "Content-Length: " << body.size() << "\r\n";
    request << "X-WebhookOps-Tunnel-Token: " << token << "\r\n";
    request << "Connection: close\r\n\r\n";
    request << body;

    const std::string text = request.str();
    if (!wops::send_all(sock, reinterpret_cast<const uint8_t*>(text.data()), text.size(), &error)) {
        wops::log_warn("tunnel", log_context + "_send_failed", {{"error", error}});
        wops::close_socket(sock);
        return 0;
    }

    std::vector<uint8_t> buffer(1024);
    const int got = wops::recv_some(sock, buffer.data(), buffer.size(), &error);
    wops::close_socket(sock);
    if (got <= 0) {
        wops::log_warn("tunnel", log_context + "_response_missing", {{"error", error}});
        return 0;
    }

    std::string response(buffer.begin(), buffer.begin() + got);
    std::istringstream status_line(response);
    std::string http_version;
    int status_code = 0;
    status_line >> http_version >> status_code;
    return status_code;
}

std::string make_auth_payload(
    const std::string& agent_id,
    const std::string& nonce,
    const std::string& proof,
    const std::map<std::string, std::string>& metadata) {
    std::ostringstream payload;
    payload << "{\"agent_id\":\"" << json_escape(agent_id) << "\",";
    payload << "\"nonce\":\"" << json_escape(nonce) << "\",";
    payload << "\"proof\":\"" << json_escape(proof) << "\",";
    payload << "\"metadata\":{";
    bool first = true;
    for (const auto& [key, value] : metadata) {
        if (!first) {
            payload << ",";
        }
        first = false;
        payload << "\"" << json_escape(key) << "\":\"" << json_escape(value) << "\"";
    }
    payload << "}}";
    return payload.str();
}

bool authenticate_agent(
    const Config& config,
    const std::string& agent_id,
    const std::string& nonce,
    const std::string& proof,
    const std::string& peer) {
    if (!config.auth_url.empty() && !config.auth_token.empty()) {
        const std::string body = make_auth_payload(agent_id, nonce, proof, {{"peer", peer}});
        const int status_code = post_json(config.auth_url, config.auth_token, body, "auth");
        if (status_code < 200 || status_code >= 300) {
            wops::log_warn(
                "tunnel",
                "agent_auth_callback_rejected",
                {{"agent", agent_id}, {"status", std::to_string(status_code)}});
            return false;
        }
        return true;
    }

    const auto expected = wops::hmac_sha256_hex(config.secret, agent_id + "\n" + nonce);
    return wops::constant_time_equals(proof, expected);
}

class Controller {
   public:
    explicit Controller(Config config) : config_(std::move(config)) {}

    bool start();
    void command_loop();
    void request_stop();
    std::shared_ptr<AgentSession> select_agent();
    void remove_agent(const std::string& id);
    void report_agent_status(
        const std::string& id,
        const std::string& event,
        std::map<std::string, std::string> metadata = {});

   private:
    void control_accept_loop();
    void proxy_accept_loop();
    void handle_proxy_client(wops::socket_t client, std::string peer);
    void print_nodes();
    void set_selected_agent(const std::string& id);
    void load_selected_agent();
    void save_selected_agent(const std::string& id);

    Config config_;
    std::atomic<bool> running_{true};
    wops::socket_t control_listener_ = wops::invalid_socket;
    wops::socket_t proxy_listener_ = wops::invalid_socket;
    std::mutex agents_mu_;
    std::map<std::string, std::shared_ptr<AgentSession>> agents_;
    std::string selected_agent_;
    std::atomic<uint32_t> next_stream_id_{1};
};

std::atomic<bool> g_stop_requested{false};

void handle_signal(int) {
    g_stop_requested.store(true);
}

void install_signal_handlers() {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
}

void AgentSession::start() {
    std::thread([self = shared_from_this()] { self->read_loop(); }).detach();
}

bool AgentSession::send(const wops::Frame& frame) {
    std::string error;
    std::lock_guard<std::mutex> lock(send_mu_);
    if (!alive_.load()) {
        return false;
    }
    if (!channel_->send_frame(frame, &error)) {
        wops::log_error("tunnel", "agent_send_failed", {{"agent", id_}, {"error", error}});
        alive_.store(false);
        return false;
    }
    return true;
}

void AgentSession::register_stream(const std::shared_ptr<ProxyStream>& stream) {
    std::lock_guard<std::mutex> lock(streams_mu_);
    streams_[stream->id] = stream;
}

void AgentSession::unregister_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mu_);
    streams_.erase(stream_id);
}

std::shared_ptr<ProxyStream> AgentSession::find_stream(uint32_t stream_id) {
    std::lock_guard<std::mutex> lock(streams_mu_);
    auto it = streams_.find(stream_id);
    if (it == streams_.end()) {
        return {};
    }
    return it->second;
}

void AgentSession::close_all_streams(const std::string& reason) {
    std::map<uint32_t, std::shared_ptr<ProxyStream>> copy;
    {
        std::lock_guard<std::mutex> lock(streams_mu_);
        copy = streams_;
        streams_.clear();
    }

    for (auto& [_, stream] : copy) {
        {
            std::lock_guard<std::mutex> lock(stream->mu);
            stream->error = reason;
            stream->closed = true;
        }
        stream->cv.notify_all();
        close_proxy_client(stream);
    }
}

void AgentSession::close() {
    const bool was_alive = alive_.exchange(false);
    if (was_alive) {
        channel_->shutdown();
        channel_->close();
    }
}

void AgentSession::read_loop() {
    while (alive_.load()) {
        std::string error;
        auto frame = channel_->recv_frame(&error);
        if (!frame) {
            if (alive_.load()) {
                wops::log_warn("tunnel", "agent_disconnected", {{"agent", id_}, {"error", error}});
            }
            break;
        }

        if (frame->type == wops::FrameType::Heartbeat) {
            owner_.report_agent_status(id_, "heartbeat");
            continue;
        }

        auto stream = find_stream(frame->stream_id);
        if (!stream) {
            continue;
        }

        if (frame->type == wops::FrameType::OpenOk) {
            {
                std::lock_guard<std::mutex> lock(stream->mu);
                stream->opened = true;
            }
            stream->cv.notify_all();
            continue;
        }

        if (frame->type == wops::FrameType::Error) {
            bool close_client = false;
            {
                std::lock_guard<std::mutex> lock(stream->mu);
                stream->error = wops::payload_text(frame->payload);
                close_client = stream->opened;
                stream->closed = true;
            }
            stream->cv.notify_all();
            if (close_client) {
                close_proxy_client(stream);
            }
            continue;
        }

        if (frame->type == wops::FrameType::Data) {
            std::string send_error;
            if (!wops::send_all(stream->client, frame->payload.data(), frame->payload.size(), &send_error)) {
                send({wops::FrameType::Close, frame->stream_id, {}});
                unregister_stream(frame->stream_id);
                close_proxy_client(stream);
            }
            continue;
        }

        if (frame->type == wops::FrameType::Close) {
            {
                std::lock_guard<std::mutex> lock(stream->mu);
                stream->closed = true;
            }
            stream->cv.notify_all();
            unregister_stream(frame->stream_id);
            close_proxy_client(stream);
        }
    }

    alive_.store(false);
    close_all_streams("agent disconnected");
    owner_.report_agent_status(id_, "offline");
    owner_.remove_agent(id_);
}

bool Controller::start() {
    load_selected_agent();

    std::string error;
    control_listener_ = wops::listen_tcp(config_.control_host, config_.control_port, 128, &error);
    if (!wops::socket_valid(control_listener_)) {
        wops::log_error("tunnel", "control_listen_failed", {{"error", error}});
        return false;
    }

    proxy_listener_ = wops::listen_tcp(config_.proxy_host, config_.proxy_port, 128, &error);
    if (!wops::socket_valid(proxy_listener_)) {
        wops::log_error("tunnel", "proxy_listen_failed", {{"error", error}});
        return false;
    }

    wops::log_info("tunnel", "control_listening", {{"endpoint", wops::endpoint_to_string(config_.control_host, config_.control_port)}});
    wops::log_info("tunnel", "proxy_listening", {{"endpoint", wops::endpoint_to_string(config_.proxy_host, config_.proxy_port)}});
    if (config_.tls.enabled) {
        wops::log_info("tunnel", "tls_enabled", {{"cert", config_.tls.cert_path}});
    }

    std::thread([this] { control_accept_loop(); }).detach();
    std::thread([this] { proxy_accept_loop(); }).detach();
    return true;
}

void Controller::control_accept_loop() {
    while (running_.load()) {
        std::string peer;
        std::string error;
        auto sock = wops::accept_tcp(control_listener_, &peer, &error);
        if (!wops::socket_valid(sock)) {
            if (running_.load()) {
                wops::log_error("tunnel", "agent_accept_failed", {{"error", error}});
            }
            continue;
        }

        auto channel = wops::Channel::server(sock, config_.tls, &error);
        if (!channel) {
            wops::log_warn("tunnel", "agent_tls_failed", {{"peer", peer}, {"error", error}});
            continue;
        }

        auto hello = channel->recv_frame(&error);
        std::string node_id;
        if (!hello || hello->type != wops::FrameType::Hello ||
            !parse_hello(wops::payload_text(hello->payload), &node_id)) {
            wops::log_warn("tunnel", "agent_rejected", {{"peer", peer}});
            channel->close();
            continue;
        }

        const auto nonce = wops::random_hex(32, &error);
        if (nonce.empty() ||
            !channel->send_frame({wops::FrameType::AuthChallenge, wops::kControlStream, wops::text_payload(nonce)}, &error)) {
            wops::log_warn("tunnel", "auth_challenge_failed", {{"peer", peer}, {"error", error}});
            channel->close();
            continue;
        }

        auto auth = channel->recv_frame(&error);
        const std::string proof = auth ? wops::payload_text(auth->payload) : "";
        if (!auth || auth->type != wops::FrameType::AuthResponse ||
            !authenticate_agent(config_, node_id, nonce, proof, peer)) {
            wops::log_warn("tunnel", "agent_auth_failed", {{"peer", peer}, {"agent", node_id}});
            channel->close();
            continue;
        }

        auto agent = std::make_shared<AgentSession>(*this, std::move(channel), node_id);
        {
            std::lock_guard<std::mutex> lock(agents_mu_);
            auto existing = agents_.find(node_id);
            if (existing != agents_.end()) {
                existing->second->close();
                agents_.erase(existing);
            }
            agents_[node_id] = agent;
            if (selected_agent_.empty()) {
                selected_agent_ = node_id;
                save_selected_agent(node_id);
            }
        }

        agent->send({wops::FrameType::HelloOk, wops::kControlStream, wops::text_payload("ok")});
        wops::log_info("tunnel", "agent_online", {{"agent", node_id}, {"peer", peer}});
        report_agent_status(node_id, "online", {{"peer", peer}});
        agent->start();
    }
}

void Controller::proxy_accept_loop() {
    while (running_.load()) {
        std::string peer;
        std::string error;
        auto client = wops::accept_tcp(proxy_listener_, &peer, &error);
        if (!wops::socket_valid(client)) {
            if (running_.load()) {
                wops::log_error("tunnel", "proxy_accept_failed", {{"error", error}});
            }
            continue;
        }
        std::thread([this, client, peer] { handle_proxy_client(client, peer); }).detach();
    }
}

std::shared_ptr<AgentSession> Controller::select_agent() {
    std::lock_guard<std::mutex> lock(agents_mu_);
    if (!selected_agent_.empty()) {
        auto it = agents_.find(selected_agent_);
        if (it != agents_.end() && it->second->alive()) {
            return it->second;
        }
        return {};
    }
    for (auto& [_, agent] : agents_) {
        if (agent->alive()) {
            selected_agent_ = agent->id();
            return agent;
        }
    }
    return {};
}

void Controller::remove_agent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agents_mu_);
    auto it = agents_.find(id);
    if (it != agents_.end()) {
        agents_.erase(it);
    }
    if (selected_agent_ == id) {
        selected_agent_.clear();
        if (!agents_.empty()) {
            selected_agent_ = agents_.begin()->first;
        }
    }
}

void Controller::report_agent_status(
    const std::string& id,
    const std::string& event,
    std::map<std::string, std::string> metadata) {
    if (config_.status_url.empty() || config_.status_token.empty()) {
        return;
    }
    const Config config = config_;
    std::thread([config, id, event, metadata = std::move(metadata)] {
        post_agent_status(config, id, event, metadata);
    }).detach();
}

void Controller::request_stop() {
    const bool was_running = running_.exchange(false);
    if (!was_running) {
        return;
    }

    wops::shutdown_socket(control_listener_);
    wops::shutdown_socket(proxy_listener_);
    wops::close_socket(control_listener_);
    wops::close_socket(proxy_listener_);

    std::map<std::string, std::shared_ptr<AgentSession>> agents;
    {
        std::lock_guard<std::mutex> lock(agents_mu_);
        agents.swap(agents_);
        selected_agent_.clear();
    }

    for (auto& [_, agent] : agents) {
        agent->close();
        agent->close_all_streams("tunnel shutting down");
    }
}

void Controller::handle_proxy_client(wops::socket_t client, std::string peer) {
    std::string header;
    std::vector<uint8_t> leftover;
    std::string error;
    if (!read_http_header(client, &header, &leftover, &error)) {
        wops::close_socket(client);
        return;
    }

    auto request = parse_proxy_request(header, leftover);
    if (!request.ok) {
        send_http_error(client, 400, request.error);
        wops::close_socket(client);
        return;
    }

    auto agent = select_agent();
    if (!agent) {
        send_http_error(client, 503, "no agent online");
        wops::close_socket(client);
        return;
    }

    const uint32_t stream_id = next_stream_id_.fetch_add(1);
    auto stream = std::make_shared<ProxyStream>();
    stream->id = stream_id;
    stream->client = client;
    agent->register_stream(stream);

    wops::log_info(
        "tunnel",
        "stream_open",
        {{"peer", peer},
         {"stream", std::to_string(stream_id)},
         {"agent", agent->id()},
         {"target", wops::endpoint_to_string(request.host, request.port)}});

    if (!agent->send({wops::FrameType::Open, stream_id, wops::text_payload(make_open_payload(request.host, request.port))})) {
        agent->unregister_stream(stream_id);
        send_http_error(client, 502, "agent send failed");
        close_proxy_client(stream);
        return;
    }

    {
        std::unique_lock<std::mutex> lock(stream->mu);
        const bool ready = stream->cv.wait_for(lock, 10s, [&] {
            return stream->opened || !stream->error.empty() || stream->closed;
        });
        if (!ready || !stream->error.empty() || !stream->opened) {
            const std::string reason = stream->error.empty() ? "agent open timeout" : stream->error;
            agent->unregister_stream(stream_id);
            send_http_error(client, 502, reason);
            close_proxy_client(stream);
            return;
        }
    }

    if (request.is_connect) {
        const std::string ok = "HTTP/1.1 200 Connection Established\r\n\r\n";
        if (!wops::send_all(client, reinterpret_cast<const uint8_t*>(ok.data()), ok.size(), &error)) {
            agent->send({wops::FrameType::Close, stream_id, {}});
            agent->unregister_stream(stream_id);
            close_proxy_client(stream);
            return;
        }
    } else if (!request.initial_data.empty()) {
        if (!agent->send({wops::FrameType::Data, stream_id, request.initial_data})) {
            agent->unregister_stream(stream_id);
            close_proxy_client(stream);
            return;
        }
    }

    std::vector<uint8_t> buffer(32 * 1024);
    while (true) {
        const int got = wops::recv_some(client, buffer.data(), buffer.size(), &error);
        if (got <= 0) {
            break;
        }
        std::vector<uint8_t> data(buffer.begin(), buffer.begin() + got);
        if (!agent->send({wops::FrameType::Data, stream_id, std::move(data)})) {
            break;
        }
    }

    agent->send({wops::FrameType::Close, stream_id, {}});
    agent->unregister_stream(stream_id);
    close_proxy_client(stream);
}

void Controller::print_nodes() {
    std::lock_guard<std::mutex> lock(agents_mu_);
    if (agents_.empty()) {
        std::cout << "no agents online\n";
        return;
    }
    for (const auto& [id, agent] : agents_) {
        const char* selected = id == selected_agent_ ? "*" : " ";
        std::cout << selected << " " << id << " " << (agent->alive() ? "online" : "offline") << "\n";
    }
}

void Controller::set_selected_agent(const std::string& id) {
    std::lock_guard<std::mutex> lock(agents_mu_);
    if (agents_.find(id) == agents_.end()) {
        std::cout << "unknown agent: " << id << "\n";
        return;
    }
    selected_agent_ = id;
    save_selected_agent(id);
    std::cout << "selected agent: " << selected_agent_ << "\n";
}

void Controller::load_selected_agent() {
    selected_agent_ = config_.initial_selected_agent;

    wops::ConfigFile state;
    std::string error;
    if (!wops::load_config_file(config_.state_path, &state, &error)) {
        return;
    }

    if (const auto saved = wops::config_get(state, "selected_agent"); !saved.empty()) {
        selected_agent_ = saved;
        wops::log_info("tunnel", "selection_restored", {{"agent", selected_agent_}, {"state", config_.state_path}});
    }
}

void Controller::save_selected_agent(const std::string& id) {
    if (config_.state_path.empty()) {
        return;
    }

    std::ofstream state(config_.state_path, std::ios::trunc);
    if (!state) {
        wops::log_warn("tunnel", "selection_save_failed", {{"state", config_.state_path}});
        return;
    }
    state << "selected_agent=" << id << "\n";
}

void Controller::command_loop() {
    std::cout << "commands: help, agents, use <agent>, status, quit\n";
    std::string line;
    while (running_.load()) {
        std::cout << "wops> " << std::flush;
        if (!std::getline(std::cin, line)) {
            while (running_.load()) {
                std::this_thread::sleep_for(1s);
            }
            break;
        }

        std::istringstream input(trim_copy(line));
        std::string command;
        input >> command;
        if (command.empty()) {
            continue;
        }
        if (command == "help") {
            std::cout << "help            show commands\n";
            std::cout << "agents          list connected agents\n";
            std::cout << "use <agent>    select active agent\n";
            std::cout << "status          show selected agent and listeners\n";
            std::cout << "quit            stop tunnel\n";
        } else if (command == "agents" || command == "nodes") {
            print_nodes();
        } else if (command == "use") {
            std::string id;
            input >> id;
            if (id.empty()) {
                std::cout << "usage: use <agent>\n";
            } else {
                set_selected_agent(id);
            }
        } else if (command == "status") {
            std::lock_guard<std::mutex> lock(agents_mu_);
            std::cout << "control: " << wops::endpoint_to_string(config_.control_host, config_.control_port) << "\n";
            std::cout << "proxy:   " << wops::endpoint_to_string(config_.proxy_host, config_.proxy_port) << "\n";
            std::cout << "active:  " << (selected_agent_.empty() ? "(none)" : selected_agent_) << "\n";
            std::cout << "state:   " << (config_.state_path.empty() ? "(disabled)" : config_.state_path) << "\n";
        } else if (command == "quit" || command == "exit") {
            request_stop();
            break;
        } else {
            std::cout << "unknown command: " << command << "\n";
        }
    }
}

void print_usage() {
    std::cout << "usage:\n";
    std::cout << "  webhookops-tunnel run [options]\n";
    std::cout << "  webhookops-tunnel show-config [options]\n";
    std::cout << "  webhookops-tunnel version\n\n";
    std::cout << "options:\n";
    std::cout << "  --config path\n";
    std::cout << "  --control host:port\n";
    std::cout << "  --proxy host:port\n";
    std::cout << "  --secret secret\n";
    std::cout << "  --token token    legacy alias for --secret\n";
    std::cout << "  --state path\n";
    std::cout << "  --selected agent\n\n";
    std::cout << "  --auth-url http://host:port/path\n";
    std::cout << "  --auth-token token\n";
    std::cout << "  --status-url http://host:port/path\n";
    std::cout << "  --status-token token\n\n";
    std::cout << "  --tls-cert path\n";
    std::cout << "  --tls-key path\n\n";
    std::cout << "default control: 0.0.0.0:9700\n";
    std::cout << "default proxy:   127.0.0.1:9080\n";
}

Command detect_command(int argc, char** argv, int* first_option) {
    *first_option = 1;
    if (argc < 2) {
        return Command::Run;
    }

    const std::string arg = argv[1];
    if (arg == "run") {
        *first_option = 2;
        return Command::Run;
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
    return Command::Run;
}

void print_resolved_config(const Config& config) {
    std::cout << "control=" << wops::endpoint_to_string(config.control_host, config.control_port) << "\n";
    std::cout << "proxy=" << wops::endpoint_to_string(config.proxy_host, config.proxy_port) << "\n";
    std::cout << "secret=(set)\n";
    std::cout << "state=" << config.state_path << "\n";
    std::cout << "tls=" << (config.tls.enabled ? "true" : "false") << "\n";
    if (config.tls.enabled) {
        std::cout << "tls_cert=" << config.tls.cert_path << "\n";
        std::cout << "tls_key=" << config.tls.key_path << "\n";
    }
    if (!config.initial_selected_agent.empty()) {
        std::cout << "selected_agent=" << config.initial_selected_agent << "\n";
    }
    if (!config.auth_url.empty()) {
        std::cout << "auth_url=" << config.auth_url << "\n";
    }
    if (!config.auth_token.empty()) {
        std::cout << "auth_token=(set)\n";
    }
    if (!config.status_url.empty()) {
        std::cout << "status_url=" << config.status_url << "\n";
    }
    if (!config.status_token.empty()) {
        std::cout << "status_token=(set)\n";
    }
}

bool apply_controller_config(const wops::ConfigFile& file, Config* config) {
    if (const auto value = wops::config_get(file, "control"); !value.empty()) {
        const auto endpoint = wops::parse_host_port(value);
        if (!endpoint) {
            std::cerr << "invalid config control endpoint\n";
            return false;
        }
        config->control_host = endpoint->first;
        config->control_port = endpoint->second;
    }

    if (const auto value = wops::config_get(file, "proxy"); !value.empty()) {
        const auto endpoint = wops::parse_host_port(value);
        if (!endpoint) {
            std::cerr << "invalid config proxy endpoint\n";
            return false;
        }
        config->proxy_host = endpoint->first;
        config->proxy_port = endpoint->second;
    }

    if (const auto value = wops::config_get(file, "secret"); !value.empty()) {
        config->secret = value;
    } else if (const auto legacy = wops::config_get(file, "token"); !legacy.empty()) {
        config->secret = legacy;
    }

    if (const auto value = wops::config_get(file, "state"); !value.empty()) {
        config->state_path = value;
    }

    if (const auto value = wops::config_get(file, "selected_agent"); !value.empty()) {
        config->initial_selected_agent = value;
    }

    if (const auto value = wops::config_get(file, "auth_url"); !value.empty()) {
        config->auth_url = value;
    }

    if (const auto value = wops::config_get(file, "auth_token"); !value.empty()) {
        config->auth_token = value;
    }

    if (const auto value = wops::config_get(file, "status_url"); !value.empty()) {
        config->status_url = value;
    }

    if (const auto value = wops::config_get(file, "status_token"); !value.empty()) {
        config->status_token = value;
    }

    if (const auto value = wops::config_get(file, "tls_cert"); !value.empty()) {
        config->tls.cert_path = value;
        config->tls.enabled = true;
    }

    if (const auto value = wops::config_get(file, "tls_key"); !value.empty()) {
        config->tls.key_path = value;
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
            return apply_controller_config(file, config);
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
        } else if (arg == "--control") {
            const auto endpoint = wops::parse_host_port(require_value(arg));
            if (!endpoint) {
                std::cerr << "invalid --control endpoint\n";
                return false;
            }
            config->control_host = endpoint->first;
            config->control_port = endpoint->second;
        } else if (arg == "--proxy") {
            const auto endpoint = wops::parse_host_port(require_value(arg));
            if (!endpoint) {
                std::cerr << "invalid --proxy endpoint\n";
                return false;
            }
            config->proxy_host = endpoint->first;
            config->proxy_port = endpoint->second;
        } else if (arg == "--secret" || arg == "--token") {
            config->secret = require_value(arg);
            if (config->secret.empty()) {
                std::cerr << arg << " cannot be empty\n";
                return false;
            }
        } else if (arg == "--state") {
            config->state_path = require_value(arg);
        } else if (arg == "--selected") {
            config->initial_selected_agent = require_value(arg);
            if (config->initial_selected_agent.empty()) {
                std::cerr << "--selected cannot be empty\n";
                return false;
            }
        } else if (arg == "--auth-url") {
            config->auth_url = require_value(arg);
        } else if (arg == "--auth-token") {
            config->auth_token = require_value(arg);
        } else if (arg == "--status-url") {
            config->status_url = require_value(arg);
        } else if (arg == "--status-token") {
            config->status_token = require_value(arg);
        } else if (arg == "--tls-cert") {
            config->tls.cert_path = require_value(arg);
            config->tls.enabled = true;
        } else if (arg == "--tls-key") {
            config->tls.key_path = require_value(arg);
            config->tls.enabled = true;
        } else {
            std::cerr << "unknown argument: " << arg << "\n";
            print_usage();
            return false;
        }
    }
    if (config->tls.enabled && (config->tls.cert_path.empty() || config->tls.key_path.empty())) {
        std::cerr << "TLS requires both --tls-cert and --tls-key\n";
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
        std::cout << "webhookops-tunnel " << kVersion << "\n";
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

    std::string error;
    if (!wops::net_init(&error)) {
        std::cerr << error << "\n";
        return 1;
    }

    Controller tunnel(config);
    if (!tunnel.start()) {
        wops::net_cleanup();
        return 1;
    }

    std::thread signal_watcher([&tunnel] {
        while (!g_stop_requested.load()) {
            std::this_thread::sleep_for(100ms);
        }
        tunnel.request_stop();
    });

    tunnel.command_loop();
    tunnel.request_stop();
    g_stop_requested.store(true);
    if (signal_watcher.joinable()) {
        signal_watcher.join();
    }
    wops::net_cleanup();
    return 0;
}
