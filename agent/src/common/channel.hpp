#pragma once

#include "common/net.hpp"
#include "common/protocol.hpp"

#include <memory>
#include <optional>
#include <string>

using SSL = struct ssl_st;
using SSL_CTX = struct ssl_ctx_st;

namespace wops {

struct TlsOptions {
    bool enabled = false;
    std::string cert_path;
    std::string key_path;
    std::string ca_path;
    bool insecure_skip_verify = false;
};

class Channel {
   public:
    explicit Channel(socket_t sock);
    ~Channel();

    Channel(const Channel&) = delete;
    Channel& operator=(const Channel&) = delete;

    static std::unique_ptr<Channel> server(socket_t sock, const TlsOptions& tls, std::string* error);
    static std::unique_ptr<Channel> client(socket_t sock, const std::string& server_name, const TlsOptions& tls, std::string* error);

    bool send_frame(const Frame& frame, std::string* error);
    std::optional<Frame> recv_frame(std::string* error);
    bool send_all(const uint8_t* data, size_t len, std::string* error);
    int recv_some(uint8_t* data, size_t len, std::string* error);

    void shutdown();
    void close();
    bool valid() const;
    bool tls_enabled() const { return ssl_ != nullptr; }

   private:
    socket_t sock_ = invalid_socket;
    SSL_CTX* ctx_ = nullptr;
    SSL* ssl_ = nullptr;
};

void openssl_init_once();

}  // namespace wops

