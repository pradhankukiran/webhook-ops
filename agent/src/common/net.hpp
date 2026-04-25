#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
namespace wops {
using socket_t = SOCKET;
constexpr socket_t invalid_socket = INVALID_SOCKET;
}
#else
namespace wops {
using socket_t = int;
constexpr socket_t invalid_socket = -1;
}
#endif

namespace wops {

bool net_init(std::string* error);
void net_cleanup();
void close_socket(socket_t sock);
void shutdown_socket(socket_t sock);
bool socket_valid(socket_t sock);

socket_t connect_tcp(const std::string& host, uint16_t port, std::string* error);
socket_t listen_tcp(const std::string& host, uint16_t port, int backlog, std::string* error);
socket_t accept_tcp(socket_t listener, std::string* peer, std::string* error);

bool send_all(socket_t sock, const uint8_t* data, size_t len, std::string* error);
int recv_some(socket_t sock, uint8_t* data, size_t len, std::string* error);

std::string last_socket_error();
std::optional<std::pair<std::string, uint16_t>> parse_host_port(const std::string& value, uint16_t default_port = 0);
std::string endpoint_to_string(const std::string& host, uint16_t port);

}  // namespace wops

