#include "../protocol/codec.hpp"
#include "../protocol/packet.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <unordered_set>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

constexpr const char* kReceiverIp = "127.0.0.1";
constexpr uint16_t kReceiverPort = 6000;
constexpr std::size_t kReceiveBufferSize = protocol::kHeaderSize + protocol::kMaxPayloadSize;
constexpr int kAckTimeoutMs = 1000;
constexpr int kMaxRetries = 3;

sockaddr_in create_receiver_address()
{
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kReceiverPort);

    if (inet_pton(AF_INET, kReceiverIp, &address.sin_addr) != 1) {
        throw std::runtime_error("invalid receiver IP address");
    }

    return address;
}

void send_packet(int fd, const protocol::Packet& packet, const sockaddr_in& destination)
{
    std::vector<uint8_t> encoded = protocol::encode_packet(packet);
    ssize_t sent = sendto(fd, encoded.data(), encoded.size(), 0, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));

    if (sent < 0) {
        throw std::runtime_error(std::string("sendto failed: ") + std::strerror(errno));
    }

    if (static_cast<std::size_t>(sent) != encoded.size()) {
        throw std::runtime_error("partial UDP datagram send");
    }
}

protocol::Packet receive_packet(int fd)
{
    std::vector<uint8_t> buffer(kReceiveBufferSize);
    ssize_t received = recvfrom(fd, buffer.data(), buffer.size(), 0, nullptr, nullptr);

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

bool wait_for_ack(
    int fd, uint32_t expected_sequence,
    std::unordered_set<uint32_t>& completed_sequences)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(kAckTimeoutMs);
    while (true) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= decltype(remaining)::zero()) {
            return false;
        }
        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int timeout_ms = static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
        const int result = poll(&pfd, 1, timeout_ms);
        if (result < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error(
                std::string("poll failed: ") + std::strerror(errno));
        }
        if (result == 0) {
            return false;
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            throw std::runtime_error("UDP socket error while waiting for ACK");
        }
        if (!(pfd.revents & POLLIN)) {
            continue;
        }
        protocol::Packet ack = receive_packet(fd);
        if (std::chrono::steady_clock::now() >= deadline) {
            return false;
        }
        if (ack.type != protocol::MessageType::Ack || !ack.payload.empty()) {
            throw std::runtime_error("expected empty ACK packet");
        }
        if (ack.sequence == expected_sequence) {
            completed_sequences.insert(ack.sequence);
            return true;
        }
        // Completion history, not numeric ordering: DATA order can be 1,3,2.
        if (completed_sequences.count(ack.sequence) != 0) {
            std::cout << "Ignored stale ACK seq=" << ack.sequence << '\n';
            continue;
        }
        throw std::runtime_error("ACK sequence mismatch");
    }
}

void send_data_with_retry(int fd, const sockaddr_in& receiver_address, uint32_t sequence, const std::string& message, std::unordered_set<uint32_t>& completed_sequences)
{
    protocol::Packet data_packet{protocol::MessageType::Data, sequence, std::vector<uint8_t>(message.begin(), message.end())};

    for (int attempt = 1; attempt <= kMaxRetries + 1; ++attempt) {
        send_packet(fd, data_packet, receiver_address);
        std::cout << "Sent DATA seq=" << sequence << " attempt=" << attempt << " payload=" << message << '\n';

        if (wait_for_ack(fd, sequence, completed_sequences)) {
            std::cout << "Received ACK seq=" << sequence << '\n';
            return;
        }

        std::cout << "ACK timeout seq=" << sequence << '\n';
    }

    throw std::runtime_error("ACK retry limit exceeded");
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        bool out_of_order = false;

        if (argc >= 2) {
            if (std::string(argv[1]) != "out-of-order") {
                throw std::runtime_error("unknown mode: " + std::string(argv[1]));
            }

            out_of_order = true;
        }

        int fd = socket(AF_INET, SOCK_DGRAM, 0);

        if (fd < 0) {
            throw std::runtime_error(std::string("socket failed: ") + std::strerror(errno));
        }

        sockaddr_in receiver_address = create_receiver_address();
        std::unordered_set<uint32_t> completed_sequences;

        if (out_of_order) {
            std::cout << "Test mode: out-of-order\n";
            send_data_with_retry(fd, receiver_address, 1, "message-1", completed_sequences);
            send_data_with_retry(fd, receiver_address, 3, "message-3", completed_sequences);
            send_data_with_retry(fd, receiver_address, 2, "message-2", completed_sequences);
        }
        else {
            send_data_with_retry(fd, receiver_address, 1, "hello udp", completed_sequences);
        }

        close(fd);
    }
    catch (const std::exception& e) {
        std::cerr << "UDP Sender error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}