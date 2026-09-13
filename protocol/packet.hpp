#pragma once

#include <cstdint>
#include <vector>

namespace protocol {

constexpr uint16_t kMagic = 0xAA55;
constexpr uint8_t kVersion = 1;
constexpr uint32_t kMaxPayloadSize = 1024;
constexpr std::size_t kHeaderSize = 16;

enum class MessageType : uint8_t {
    Data = 1,
    Ack = 2,
    Heartbeat = 3,
    HeartbeatAck = 4,
};

struct Packet {
    MessageType type;
    uint32_t sequence;
    std::vector<uint8_t> payload;
};

} // namespace protocol
