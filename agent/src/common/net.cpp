#include "common/net.hpp"

#include <cstring>
#include <sstream>

#ifdef _WIN32
#include <mstcpip.h>
#else
#include <arpa/inet.h>
#include <cerrno>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

namespace wops {

bool net_init(std::string* error) {
#ifdef _WIN32
    WSADATA data{};
    const int rc = WSAStartup(MAKEWORD(2, 2), &data);
    if (rc != 0) {
        if (error) {
            *error = "WSAStartup failed: " + std::to_string(rc);
        }
        return false;
    }
#else
    (void)error;
#endif
    return true;
}

void net_cleanup() {
#ifdef _WIN32
    WSACleanup();
#endif
}

bool socket_valid(socket_t sock) {
    return sock != invalid_socket;
}

void close_socket(socket_t sock) {
    if (!socket_valid(sock)) {
        return;
    }
#ifdef _WIN32
    closesocket(sock);
#else
    close(sock);
#endif
}

void shutdown_socket(socket_t sock) {
    if (!socket_valid(sock)) {
        return;
    }
#ifdef _WIN32
    shutdown(sock, SD_BOTH);
#else
    shutdown(sock, SHUT_RDWR);
#endif
}

std::string last_socket_error() {
#ifdef _WIN32
    return "socket error " + std::to_string(WSAGetLastError());
#else
    return std::strerror(errno);
#endif
}

socket_t connect_tcp(const std::string& host, uint16_t port, std::string* error) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;

    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    const int rc = getaddrinfo(host.c_str(), port_text.c_str(), &hints, &result);
    if (rc != 0) {
        if (error) {
#ifdef _WIN32
            *error = "getaddrinfo failed";
#else
            *error = gai_strerror(rc);
#endif
        }
        return invalid_socket;
    }

    socket_t connected = invalid_socket;
    std::string last_error;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        socket_t sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!socket_valid(sock)) {
            last_error = last_socket_error();
            continue;
        }

        if (::connect(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0) {
            connected = sock;
            break;
        }

        last_error = last_socket_error();
        close_socket(sock);
    }

    freeaddrinfo(result);
    if (!socket_valid(connected) && error) {
        *error = last_error.empty() ? "connect failed" : last_error;
    }
    return connected;
}

socket_t listen_tcp(const std::string& host, uint16_t port, int backlog, std::string* error) {
    addrinfo hints{};
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_family = AF_UNSPEC;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* result = nullptr;
    const std::string port_text = std::to_string(port);
    const char* host_ptr = host.empty() ? nullptr : host.c_str();
    const int rc = getaddrinfo(host_ptr, port_text.c_str(), &hints, &result);
    if (rc != 0) {
        if (error) {
#ifdef _WIN32
            *error = "getaddrinfo failed";
#else
            *error = gai_strerror(rc);
#endif
        }
        return invalid_socket;
    }

    socket_t listener = invalid_socket;
    std::string last_error;
    for (addrinfo* ai = result; ai != nullptr; ai = ai->ai_next) {
        socket_t sock = ::socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (!socket_valid(sock)) {
            last_error = last_socket_error();
            continue;
        }

        int enabled = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enabled), sizeof(enabled));

        if (::bind(sock, ai->ai_addr, static_cast<int>(ai->ai_addrlen)) == 0 &&
            ::listen(sock, backlog) == 0) {
            listener = sock;
            break;
        }

        last_error = last_socket_error();
        close_socket(sock);
    }

    freeaddrinfo(result);
    if (!socket_valid(listener) && error) {
        *error = last_error.empty() ? "listen failed" : last_error;
    }
    return listener;
}

socket_t accept_tcp(socket_t listener, std::string* peer, std::string* error) {
    sockaddr_storage addr{};
    socklen_t addr_len = sizeof(addr);
    socket_t sock = ::accept(listener, reinterpret_cast<sockaddr*>(&addr), &addr_len);
    if (!socket_valid(sock)) {
        if (error) {
            *error = last_socket_error();
        }
        return invalid_socket;
    }

    if (peer) {
        char host[NI_MAXHOST]{};
        char service[NI_MAXSERV]{};
        const int rc = getnameinfo(
            reinterpret_cast<sockaddr*>(&addr),
            addr_len,
            host,
            sizeof(host),
            service,
            sizeof(service),
            NI_NUMERICHOST | NI_NUMERICSERV);
        if (rc == 0) {
            *peer = std::string(host) + ":" + service;
        }
    }
    return sock;
}

bool send_all(socket_t sock, const uint8_t* data, size_t len, std::string* error) {
    size_t sent_total = 0;
    while (sent_total < len) {
        const size_t remaining = len - sent_total;
        const int chunk = remaining > 65536 ? 65536 : static_cast<int>(remaining);
        const int sent = ::send(sock, reinterpret_cast<const char*>(data + sent_total), chunk, 0);
        if (sent <= 0) {
            if (error) {
                *error = last_socket_error();
            }
            return false;
        }
        sent_total += static_cast<size_t>(sent);
    }
    return true;
}

int recv_some(socket_t sock, uint8_t* data, size_t len, std::string* error) {
    const int wanted = len > 65536 ? 65536 : static_cast<int>(len);
    const int got = ::recv(sock, reinterpret_cast<char*>(data), wanted, 0);
    if (got < 0 && error) {
        *error = last_socket_error();
    }
    return got;
}

std::optional<std::pair<std::string, uint16_t>> parse_host_port(const std::string& value, uint16_t default_port) {
    if (value.empty()) {
        return std::nullopt;
    }

    std::string host;
    std::string port_text;

    if (value.front() == '[') {
        const auto end = value.find(']');
        if (end == std::string::npos) {
            return std::nullopt;
        }
        host = value.substr(1, end - 1);
        if (end + 1 < value.size()) {
            if (value[end + 1] != ':') {
                return std::nullopt;
            }
            port_text = value.substr(end + 2);
        }
    } else {
        const auto colon = value.rfind(':');
        if (colon == std::string::npos) {
            host = value;
        } else {
            host = value.substr(0, colon);
            port_text = value.substr(colon + 1);
        }
    }

    if (host.empty()) {
        return std::nullopt;
    }

    uint16_t port = default_port;
    if (!port_text.empty()) {
        int parsed = 0;
        try {
            parsed = std::stoi(port_text);
        } catch (...) {
            return std::nullopt;
        }
        if (parsed <= 0 || parsed > 65535) {
            return std::nullopt;
        }
        port = static_cast<uint16_t>(parsed);
    }

    if (port == 0) {
        return std::nullopt;
    }

    return std::make_pair(host, port);
}

std::string endpoint_to_string(const std::string& host, uint16_t port) {
    return host + ":" + std::to_string(port);
}

}  // namespace wops

