#pragma once

#include "packet.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace protocol {

std::vector<uint8_t> encode_packet(const Packet& packet);

Packet decode_packet(
    const uint8_t* header,
    std::size_t header_size,
    const uint8_t* payload,
    std::size_t payload_size
);

uint32_t get_payload_length(
    const uint8_t* header,
    std::size_t header_size
);

} // namespace protocol
