#include "common/channel.hpp"

#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509_vfy.h>

#include <array>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace wops {

namespace {

std::string openssl_error() {
    const unsigned long code = ERR_get_error();
    if (code == 0) {
        return "OpenSSL error";
    }
    char buffer[256]{};
    ERR_error_string_n(code, buffer, sizeof(buffer));
    return buffer;
}

bool recv_exact(Channel* channel, uint8_t* data, size_t len, std::string* error) {
    size_t total = 0;
    while (total < len) {
        const int got = channel->recv_some(data + total, len - total, error);
        if (got == 0) {
            if (error) {
                *error = "connection closed";
            }
            return false;
        }
        if (got < 0) {
            return false;
        }
        total += static_cast<size_t>(got);
    }
    return true;
}

}  // namespace

void openssl_init_once() {
    static const bool initialized = [] {
        SSL_library_init();
        SSL_load_error_strings();
        OpenSSL_add_ssl_algorithms();
        return true;
    }();
    (void)initialized;
}

Channel::Channel(socket_t sock) : sock_(sock) {}

Channel::~Channel() {
    close();
}

std::unique_ptr<Channel> Channel::server(socket_t sock, const TlsOptions& tls, std::string* error) {
    auto channel = std::make_unique<Channel>(sock);
    if (!tls.enabled) {
        return channel;
    }

    openssl_init_once();
    channel->ctx_ = SSL_CTX_new(TLS_server_method());
    if (!channel->ctx_) {
        if (error) {
            *error = openssl_error();
        }
        return {};
    }

    if (SSL_CTX_use_certificate_file(channel->ctx_, tls.cert_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_use_PrivateKey_file(channel->ctx_, tls.key_path.c_str(), SSL_FILETYPE_PEM) != 1 ||
        SSL_CTX_check_private_key(channel->ctx_) != 1) {
        if (error) {
            *error = openssl_error();
        }
        return {};
    }

    channel->ssl_ = SSL_new(channel->ctx_);
    if (!channel->ssl_) {
        if (error) {
            *error = openssl_error();
        }
        return {};
    }
    SSL_set_fd(channel->ssl_, static_cast<int>(sock));

    if (SSL_accept(channel->ssl_) != 1) {
        if (error) {
            *error = openssl_error();
        }
        return {};
    }

    return channel;
}

std::unique_ptr<Channel> Channel::client(socket_t sock, const std::string& server_name, const TlsOptions& tls, std::string* error) {
    auto channel = std::make_unique<Channel>(sock);
    if (!tls.enabled) {
        return channel;
    }

    openssl_init_once();
    channel->ctx_ = SSL_CTX_new(TLS_client_method());
    if (!channel->ctx_) {
        if (error) {
            *error = openssl_error();
        }
        return {};
    }

    if (tls.insecure_skip_verify) {
        SSL_CTX_set_verify(channel->ctx_, SSL_VERIFY_NONE, nullptr);
    } else {
        SSL_CTX_set_verify(channel->ctx_, SSL_VERIFY_PEER, nullptr);
        if (!tls.ca_path.empty()) {
            if (SSL_CTX_load_verify_locations(channel->ctx_, tls.ca_path.c_str(), nullptr) != 1) {
                if (error) {
                    *error = openssl_error();
                }
                return {};
            }
        } else if (SSL_CTX_set_default_verify_paths(channel->ctx_) != 1) {
            if (error) {
                *error = openssl_error();
            }
            return {};
        }
    }

    channel->ssl_ = SSL_new(channel->ctx_);
    if (!channel->ssl_) {
        if (error) {
            *error = openssl_error();
        }
        return {};
    }

    SSL_set_fd(channel->ssl_, static_cast<int>(sock));
    SSL_set_tlsext_host_name(channel->ssl_, server_name.c_str());
    if (!tls.insecure_skip_verify) {
        SSL_set1_host(channel->ssl_, server_name.c_str());
    }

    if (SSL_connect(channel->ssl_) != 1) {
        if (error) {
            *error = openssl_error();
        }
        return {};
    }

    if (!tls.insecure_skip_verify && SSL_get_verify_result(channel->ssl_) != X509_V_OK) {
        if (error) {
            *error = "TLS certificate verification failed";
        }
        return {};
    }

    return channel;
}

bool Channel::send_frame(const Frame& frame, std::string* error) {
    if (frame.payload.size() > kMaxFramePayload) {
        if (error) {
            *error = "frame payload too large";
        }
        return false;
    }

    std::array<uint8_t, 9> header{};
    header[0] = static_cast<uint8_t>(frame.type);
    const uint32_t stream_be = htonl(frame.stream_id);
    const uint32_t len_be = htonl(static_cast<uint32_t>(frame.payload.size()));
    std::memcpy(header.data() + 1, &stream_be, sizeof(stream_be));
    std::memcpy(header.data() + 5, &len_be, sizeof(len_be));

    if (!send_all(header.data(), header.size(), error)) {
        return false;
    }
    if (!frame.payload.empty()) {
        return send_all(frame.payload.data(), frame.payload.size(), error);
    }
    return true;
}

std::optional<Frame> Channel::recv_frame(std::string* error) {
    std::array<uint8_t, 9> header{};
    if (!recv_exact(this, header.data(), header.size(), error)) {
        return std::nullopt;
    }

    uint32_t stream_be = 0;
    uint32_t len_be = 0;
    std::memcpy(&stream_be, header.data() + 1, sizeof(stream_be));
    std::memcpy(&len_be, header.data() + 5, sizeof(len_be));

    Frame frame;
    frame.type = static_cast<FrameType>(header[0]);
    frame.stream_id = ntohl(stream_be);
    const uint32_t len = ntohl(len_be);
    if (len > kMaxFramePayload) {
        if (error) {
            *error = "frame payload too large";
        }
        return std::nullopt;
    }

    frame.payload.resize(len);
    if (len > 0 && !recv_exact(this, frame.payload.data(), frame.payload.size(), error)) {
        return std::nullopt;
    }
    return frame;
}

bool Channel::send_all(const uint8_t* data, size_t len, std::string* error) {
    if (!ssl_) {
        return wops::send_all(sock_, data, len, error);
    }

    size_t sent_total = 0;
    while (sent_total < len) {
        const size_t remaining = len - sent_total;
        const int chunk = remaining > 65536 ? 65536 : static_cast<int>(remaining);
        const int sent = SSL_write(ssl_, data + sent_total, chunk);
        if (sent <= 0) {
            if (error) {
                *error = openssl_error();
            }
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

int Channel::recv_some(uint8_t* data, size_t len, std::string* error) {
    if (!ssl_) {
        return wops::recv_some(sock_, data, len, error);
    }

    const int wanted = len > 65536 ? 65536 : static_cast<int>(len);
    const int got = SSL_read(ssl_, data, wanted);
    if (got <= 0) {
        const int ssl_error = SSL_get_error(ssl_, got);
        if (ssl_error == SSL_ERROR_ZERO_RETURN) {
            return 0;
        }
        if (error) {
            *error = openssl_error();
        }
        return -1;
    }
    return got;
}

void Channel::shutdown() {
    if (ssl_) {
        SSL_shutdown(ssl_);
    }
    shutdown_socket(sock_);
}

void Channel::close() {
    if (ssl_) {
        SSL_free(ssl_);
        ssl_ = nullptr;
    }
    if (ctx_) {
        SSL_CTX_free(ctx_);
        ctx_ = nullptr;
    }
    close_socket(sock_);
    sock_ = invalid_socket;
}

bool Channel::valid() const {
    return socket_valid(sock_);
}

}  // namespace wops

