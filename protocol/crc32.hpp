#pragma once

#include <cstddef>
#include <cstdint>

namespace protocol {

uint32_t crc32(const uint8_t* data, std::size_t size);

} // namespace protocol
