#include "codec.hpp"
#include "crc32.hpp"

#include <arpa/inet.h>

#include <cstring>
#include <stdexcept>
#include <vector>

namespace protocol {

namespace {

void append_u16(std::vector<uint8_t>& buffer, uint16_t value)
{
    uint16_t network_value = htons(value);

    const auto* bytes =
        reinterpret_cast<const uint8_t*>(&network_value);

    buffer.insert(buffer.end(), bytes, bytes + sizeof(network_value));
}

void append_u32(std::vector<uint8_t>& buffer, uint32_t value)
{
    uint32_t network_value = htonl(value);

    const auto* bytes =
        reinterpret_cast<const uint8_t*>(&network_value);

    buffer.insert(buffer.end(), bytes, bytes + sizeof(network_value));
}

uint16_t read_u16(const uint8_t* data)
{
    uint16_t network_value;

    std::memcpy(
        &network_value,
        data,
        sizeof(network_value)
    );

    return ntohs(network_value);
}

uint32_t read_u32(const uint8_t* data)
{
    uint32_t network_value;

    std::memcpy(
        &network_value,
        data,
        sizeof(network_value)
    );

    return ntohl(network_value);
}

std::vector<uint8_t> make_crc_input(
    uint16_t magic,
    uint8_t version,
    MessageType type,
    uint32_t sequence,
    uint32_t payload_length,
    const uint8_t* payload,
    std::size_t payload_size
)
{
    std::vector<uint8_t> buffer;

    append_u16(buffer, magic);

    buffer.push_back(version);
    buffer.push_back(static_cast<uint8_t>(type));

    append_u32(buffer, sequence);
    append_u32(buffer, payload_length);

    buffer.insert(
        buffer.end(),
        payload,
        payload + payload_size
    );

    return buffer;
}

} // namespace

std::vector<uint8_t> encode_packet(const Packet& packet)
{
    if (packet.payload.size() > kMaxPayloadSize) {
        throw std::runtime_error("payload too large");
    }

    uint32_t payload_length =
        static_cast<uint32_t>(packet.payload.size());

    std::vector<uint8_t> crc_input =
        make_crc_input(
            kMagic,
            kVersion,
            packet.type,
            packet.sequence,
            payload_length,
            packet.payload.data(),
            packet.payload.size()
        );

    uint32_t checksum =
        crc32(crc_input.data(), crc_input.size());

    std::vector<uint8_t> buffer;

    append_u16(buffer, kMagic);

    buffer.push_back(kVersion);
    buffer.push_back(static_cast<uint8_t>(packet.type));

    append_u32(buffer, packet.sequence);
    append_u32(buffer, payload_length);
    append_u32(buffer, checksum);

    buffer.insert(
        buffer.end(),
        packet.payload.begin(),
        packet.payload.end()
    );

    return buffer;
}

uint32_t get_payload_length(
    const uint8_t* header,
    std::size_t header_size
)
{
    if (header_size != kHeaderSize) {
        throw std::runtime_error("invalid header size");
    }

    uint16_t magic = read_u16(header);

    if (magic != kMagic) {
        throw std::runtime_error("invalid magic");
    }

    uint8_t version = header[2];

    if (version != kVersion) {
        throw std::runtime_error("unsupported protocol version");
    }

    uint32_t payload_length =
        read_u32(header + 8);

    if (payload_length > kMaxPayloadSize) {
        throw std::runtime_error("payload too large");
    }

    return payload_length;
}

Packet decode_packet(
    const uint8_t* header,
    std::size_t header_size,
    const uint8_t* payload,
    std::size_t payload_size
)
{
    if (header_size != kHeaderSize) {
        throw std::runtime_error("invalid header size");
    }

    uint16_t magic = read_u16(header);

    if (magic != kMagic) {
        throw std::runtime_error("invalid magic");
    }

    uint8_t version = header[2];

    if (version != kVersion) {
        throw std::runtime_error("unsupported protocol version");
    }

    MessageType type =
        static_cast<MessageType>(header[3]);

    uint32_t sequence =
        read_u32(header + 4);

    uint32_t payload_length =
        read_u32(header + 8);

    uint32_t received_crc =
        read_u32(header + 12);

    if (payload_length != payload_size) {
        throw std::runtime_error("payload length mismatch");
    }

    if (payload_length > kMaxPayloadSize) {
        throw std::runtime_error("payload too large");
    }

    std::vector<uint8_t> crc_input =
        make_crc_input(
            magic,
            version,
            type,
            sequence,
            payload_length,
            payload,
            payload_size
        );

    uint32_t calculated_crc =
        crc32(crc_input.data(), crc_input.size());

    if (received_crc != calculated_crc) {
        throw std::runtime_error("CRC mismatch");
    }

    Packet packet;

    packet.type = type;
    packet.sequence = sequence;

    packet.payload.assign(
        payload,
        payload + payload_size
    );

    return packet;
}

} // namespace protocol
