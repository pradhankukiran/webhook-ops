#include "common/protocol.hpp"

#include <array>
#include <cstring>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#endif

namespace wops {

namespace {

bool recv_exact(socket_t sock, uint8_t* data, size_t len, std::string* error) {
    size_t total = 0;
    while (total < len) {
        const int got = recv_some(sock, data + total, len - total, error);
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

bool send_frame(socket_t sock, const Frame& frame, std::string* error) {
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

    if (!send_all(sock, header.data(), header.size(), error)) {
        return false;
    }
    if (!frame.payload.empty()) {
        return send_all(sock, frame.payload.data(), frame.payload.size(), error);
    }
    return true;
}

std::optional<Frame> recv_frame(socket_t sock, std::string* error) {
    std::array<uint8_t, 9> header{};
    if (!recv_exact(sock, header.data(), header.size(), error)) {
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
    if (len > 0 && !recv_exact(sock, frame.payload.data(), frame.payload.size(), error)) {
        return std::nullopt;
    }

    return frame;
}

std::vector<uint8_t> text_payload(const std::string& value) {
    return std::vector<uint8_t>(value.begin(), value.end());
}

std::string payload_text(const std::vector<uint8_t>& payload) {
    return std::string(payload.begin(), payload.end());
}

std::string frame_type_name(FrameType type) {
    switch (type) {
        case FrameType::Hello:
            return "HELLO";
        case FrameType::HelloOk:
            return "HELLO_OK";
        case FrameType::Heartbeat:
            return "HEARTBEAT";
        case FrameType::Open:
            return "OPEN";
        case FrameType::OpenOk:
            return "OPEN_OK";
        case FrameType::Data:
            return "DATA";
        case FrameType::Close:
            return "CLOSE";
        case FrameType::Error:
            return "ERROR";
        case FrameType::AuthChallenge:
            return "AUTH_CHALLENGE";
        case FrameType::AuthResponse:
            return "AUTH_RESPONSE";
    }
    return "UNKNOWN";
}

}  // namespace wops
