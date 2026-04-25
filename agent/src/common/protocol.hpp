#pragma once

#include "common/net.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace wops {

enum class FrameType : uint8_t {
    Hello = 1,
    HelloOk = 2,
    Heartbeat = 3,
    Open = 4,
    OpenOk = 5,
    Data = 6,
    Close = 7,
    Error = 8,
    AuthChallenge = 9,
    AuthResponse = 10,
};

struct Frame {
    FrameType type{};
    uint32_t stream_id{};
    std::vector<uint8_t> payload;
};

constexpr uint32_t kControlStream = 0;
constexpr uint32_t kMaxFramePayload = 16 * 1024 * 1024;

bool send_frame(socket_t sock, const Frame& frame, std::string* error);
std::optional<Frame> recv_frame(socket_t sock, std::string* error);

std::vector<uint8_t> text_payload(const std::string& value);
std::string payload_text(const std::vector<uint8_t>& payload);
std::string frame_type_name(FrameType type);

}  // namespace wops
