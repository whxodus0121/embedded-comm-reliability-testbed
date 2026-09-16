#include "../protocol/codec.hpp"
#include "../protocol/packet.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace {

constexpr uint16_t kPort = 6000;
constexpr std::size_t kReceiveBufferSize = protocol::kHeaderSize + protocol::kMaxPayloadSize;

int create_socket()
{
    int fd = socket(AF_INET, SOCK_DGRAM, 0);

    if (fd < 0) {
        throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);
        throw std::runtime_error(std::string("bind failed: ") + std::strerror(errno));
    }

    return fd;
}

protocol::Packet receive_packet(int fd, sockaddr_in& sender_address, socklen_t& sender_address_length)
{
    std::vector<uint8_t> buffer(kReceiveBufferSize);
    ssize_t received = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&sender_address), &sender_address_length);

    if (received < 0) {
        throw std::runtime_error(std::string("recvfrom failed: ") + std::strerror(errno));
    }

    if (static_cast<std::size_t>(received) < protocol::kHeaderSize) {
        throw std::runtime_error("UDP datagram smaller than protocol header");
    }

    uint32_t payload_length = protocol::get_payload_length(buffer.data(), protocol::kHeaderSize);
    std::size_t expected_size = protocol::kHeaderSize + payload_length;

    if (static_cast<std::size_t>(received) != expected_size) {
        throw std::runtime_error("UDP datagram size mismatch");
    }

    return protocol::decode_packet(buffer.data(), protocol::kHeaderSize, buffer.data() + protocol::kHeaderSize, payload_length);
}

void send_packet(int fd, const protocol::Packet& packet, const sockaddr_in& destination, socklen_t destination_length)
{
    std::vector<uint8_t> encoded = protocol::encode_packet(packet);
    ssize_t sent = sendto(fd, encoded.data(), encoded.size(), 0, reinterpret_cast<const sockaddr*>(&destination), destination_length);

    if (sent < 0) {
        throw std::runtime_error(std::string("sendto failed: ") + std::strerror(errno));
    }

    if (static_cast<std::size_t>(sent) != encoded.size()) {
        throw std::runtime_error("partial UDP datagram send");
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        bool drop_first_ack = false;
        bool out_of_order = false;

        if (argc >= 2) {
            std::string mode = argv[1];

            if (mode == "drop-first-ack") {
                drop_first_ack = true;
            }
            else if (mode == "out-of-order") {
                out_of_order = true;
            }
            else {
                throw std::runtime_error("unknown mode: " + mode);
            }
        }

        int fd = create_socket();

        std::cout << "UDP Receiver listening on port " << kPort << '\n';

        if (drop_first_ack) {
            std::cout << "Test mode: drop-first-ack\n";
        }

        if (out_of_order) {
            std::cout << "Test mode: out-of-order\n";
        }

        std::unordered_set<uint32_t> processed_sequences;
        uint32_t expected_sequence = 1;
        int unique_received = 0;
        bool ack_dropped = false;

        while (true) {
            sockaddr_in sender_address{};
            socklen_t sender_address_length = sizeof(sender_address);

            protocol::Packet packet = receive_packet(fd, sender_address, sender_address_length);

            if (packet.type != protocol::MessageType::Data) {
                throw std::runtime_error("expected DATA packet");
            }

            bool duplicate = processed_sequences.find(packet.sequence) != processed_sequences.end();

            if (duplicate) {
                std::cout << "Duplicate DATA seq=" << packet.sequence << " ignored\n";
            }
            else {
                if (packet.sequence > expected_sequence) {
                    std::cout << "Out-of-order DATA seq=" << packet.sequence << " expected=" << expected_sequence << '\n';
                }

                std::string message(packet.payload.begin(), packet.payload.end());
                std::cout << "Received DATA seq=" << packet.sequence << " payload=" << message << '\n';

                processed_sequences.insert(packet.sequence);
                ++unique_received;

                while (processed_sequences.find(expected_sequence) != processed_sequences.end()) {
                    ++expected_sequence;
                }
            }

            protocol::Packet ack{protocol::MessageType::Ack, packet.sequence, {}};

            if (drop_first_ack && !ack_dropped) {
                std::cout << "ACK intentionally dropped seq=" << packet.sequence << '\n';
                ack_dropped = true;
                continue;
            }

            send_packet(fd, ack, sender_address, sender_address_length);
            std::cout << "Sent ACK seq=" << ack.sequence << '\n';

            if (out_of_order && unique_received >= 3) {
                break;
            }

            if (drop_first_ack && duplicate) {
                break;
            }

            if (!drop_first_ack && !out_of_order) {
                break;
            }
        }

        close(fd);
    }
    catch (const std::exception& e) {
        std::cerr << "UDP Receiver error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}