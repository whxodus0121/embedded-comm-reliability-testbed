#include "../protocol/codec.hpp"
#include "../protocol/packet.hpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <set>
#include <utility>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr const char* kServerIp = "127.0.0.1";
constexpr uint16_t kServerPort = 5000;

constexpr int kAckTimeoutMs = 1000;
constexpr int kMaxRetries = 3;

constexpr int kHeartbeatIntervalSec = 2;
constexpr int kHeartbeatTimeoutMs = 1000;
constexpr int kHeartbeatCount = 3;

constexpr int kReconnectDelaySec = 2;
constexpr int kMaxReconnectAttempts = 3;

class ConnectionLost : public std::runtime_error {
public:
    explicit ConnectionLost(const std::string& message)
        : std::runtime_error(message)
    {
    }
};

void send_all(int fd, const void* data, std::size_t size)
{
    const auto* buffer = static_cast<const uint8_t*>(data);
    std::size_t total_sent = 0;

    while (total_sent < size) {
        ssize_t sent = send(fd, buffer + total_sent, size - total_sent, 0);

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            if (errno == EPIPE || errno == ECONNRESET) {
                throw ConnectionLost(std::strerror(errno));
            }

            throw std::runtime_error(
                std::string("send failed: ") + std::strerror(errno));
        }

        if (sent == 0) {
            throw ConnectionLost("connection closed while sending");
        }

        total_sent += static_cast<std::size_t>(sent);
    }
}

using Deadline = std::chrono::steady_clock::time_point;
using CompletedResponses = std::set<std::pair<protocol::MessageType, uint32_t>>;

class ResponseTimeout {};

std::vector<uint8_t> recv_exact(
    int fd, std::size_t size, Deadline deadline, std::size_t& frame_bytes)
{
    std::vector<uint8_t> buffer(size);
    std::size_t total_received = 0;

    while (total_received < size) {
        const auto remaining = deadline - std::chrono::steady_clock::now();
        if (remaining <= Deadline::duration::zero()) {
            throw ResponseTimeout();
        }

        pollfd pfd{};
        pfd.fd = fd;
        pfd.events = POLLIN;
        const int timeout_ms = static_cast<int>(
            std::chrono::ceil<std::chrono::milliseconds>(remaining).count());
        const int result = poll(&pfd, 1, timeout_ms);
        if (result < 0) {
            if (errno == EINTR) {
                continue; // Recompute remaining time; never restart the deadline.
            }
            throw std::runtime_error(
                std::string("poll failed: ") + std::strerror(errno));
        }
        if (result == 0) {
            throw ResponseTimeout();
        }
        if (pfd.revents & (POLLERR | POLLNVAL)) {
            throw ConnectionLost("connection error while waiting for response");
        }
        if (!(pfd.revents & POLLIN)) {
            if (pfd.revents & POLLHUP) {
                throw ConnectionLost("peer disconnected");
            }
            continue;
        }

        const ssize_t received = recv(
            fd, buffer.data() + total_received, size - total_received,
            MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                continue;
            }
            if (errno == ECONNRESET) {
                throw ConnectionLost(std::strerror(errno));
            }
            throw std::runtime_error(
                std::string("recv failed: ") + std::strerror(errno));
        }
        if (received == 0) {
            throw ConnectionLost("peer disconnected");
        }
        total_received += static_cast<std::size_t>(received);
        frame_bytes += static_cast<std::size_t>(received);
    }
    return buffer;
}

void send_packet(int fd, const protocol::Packet& packet)
{
    std::vector<uint8_t> encoded = protocol::encode_packet(packet);
    send_all(fd, encoded.data(), encoded.size());
}

protocol::Packet receive_packet(
    int fd, Deadline deadline, std::size_t& frame_bytes)
{
    std::vector<uint8_t> header =
        recv_exact(fd, protocol::kHeaderSize, deadline, frame_bytes);
    uint32_t payload_length =
        protocol::get_payload_length(header.data(), header.size());
    std::vector<uint8_t> payload =
        recv_exact(fd, payload_length, deadline, frame_bytes);
    return protocol::decode_packet(
        header.data(), header.size(), payload.data(), payload.size());
}

bool wait_for_response(
    int fd,
    protocol::MessageType expected_type,
    uint32_t expected_sequence,
    int timeout_ms,
    CompletedResponses& completed)
{
    const auto deadline = std::chrono::steady_clock::now()
        + std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        std::size_t frame_bytes = 0;
        protocol::Packet response;
        try {
            response = receive_packet(fd, deadline, frame_bytes);
        }
        catch (const ResponseTimeout&) {
            if (frame_bytes != 0) {
                // Discard a truncated frame's connection; retrying on this stream
                // would interpret the remaining bytes as a new header.
                shutdown(fd, SHUT_RDWR);
                close(fd);
                throw std::runtime_error(
                    "partial response deadline exceeded; connection discarded");
            }
            return false; // At a frame boundary, same-connection retry is safe.
        }
        if (std::chrono::steady_clock::now() >= deadline) {
            return false; // A complete frame was consumed, but arrived too late.
        }
        if ((response.type != protocol::MessageType::Ack &&
             response.type != protocol::MessageType::HeartbeatAck) ||
            !response.payload.empty()) {
            throw std::runtime_error("invalid response packet");
        }
        if (response.type == expected_type &&
            response.sequence == expected_sequence) {
            completed.emplace(response.type, response.sequence);
            return true;
        }
        if (completed.count({response.type, response.sequence}) != 0) {
            std::cout << "Ignored stale response seq="
                      << response.sequence << '\n';
            continue; // Same absolute deadline, even for many stale responses.
        }
        throw std::runtime_error("unexpected response type or sequence");
    }
    return false;
}

int connect_to_receiver()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        throw std::runtime_error(
            std::string("socket failed: ") + std::strerror(errno));
    }

    sockaddr_in server_address{};
    server_address.sin_family = AF_INET;
    server_address.sin_port = htons(kServerPort);

    if (inet_pton(AF_INET, kServerIp, &server_address.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("invalid server IP address");
    }

    if (connect(
            fd,
            reinterpret_cast<sockaddr*>(&server_address),
            sizeof(server_address)) < 0) {

        close(fd);

        throw std::runtime_error(
            std::string("connect failed: ") + std::strerror(errno));
    }

    return fd;
}

int connect_with_retry()
{
    for (int attempt = 1; attempt <= kMaxReconnectAttempts; ++attempt) {
        try {
            int fd = connect_to_receiver();

            std::cout
                << "Connected to receiver"
                << " attempt=" << attempt
                << '\n';

            return fd;
        }
        catch (const std::exception& e) {
            std::cout
                << "Connect failed"
                << " attempt=" << attempt
                << " error=" << e.what()
                << '\n';

            if (attempt == kMaxReconnectAttempts) {
                throw;
            }

            std::this_thread::sleep_for(
                std::chrono::seconds(kReconnectDelaySec));
        }
    }

    throw std::runtime_error("reconnect failed");
}

} // namespace

int main()
{
    try {
        int fd = connect_with_retry();
        CompletedResponses completed_responses;

        std::string message = "hello embedded";

        protocol::Packet data_packet{
            protocol::MessageType::Data,
            1,
            std::vector<uint8_t>(message.begin(), message.end())
        };

        bool acknowledged = false;

        for (int attempt = 1; attempt <= kMaxRetries + 1; ++attempt) {
            send_packet(fd, data_packet);

            std::cout
                << "Sent DATA"
                << " seq=" << data_packet.sequence
                << " attempt=" << attempt
                << " payload=" << message
                << '\n';

            if (wait_for_response(
                    fd,
                    protocol::MessageType::Ack,
                    data_packet.sequence,
                    kAckTimeoutMs, completed_responses)) {

                std::cout
                    << "Received ACK"
                    << " seq=" << data_packet.sequence
                    << '\n';

                acknowledged = true;
                break;
            }

            std::cout
                << "ACK timeout"
                << " seq=" << data_packet.sequence
                << '\n';
        }

        if (!acknowledged) {
            throw std::runtime_error("ACK retry limit exceeded");
        }

        uint32_t heartbeat_sequence = 2;

        for (int count = 1; count <= kHeartbeatCount; ++count) {
            std::this_thread::sleep_for(
                std::chrono::seconds(kHeartbeatIntervalSec));

            protocol::Packet heartbeat{
                protocol::MessageType::Heartbeat,
                heartbeat_sequence,
                {}
            };

            bool reconnect_required = false;

            try {
                send_packet(fd, heartbeat);

                std::cout
                    << "Sent HEARTBEAT"
                    << " seq=" << heartbeat.sequence
                    << " count=" << count
                    << '\n';

                if (!wait_for_response(
                        fd,
                        protocol::MessageType::HeartbeatAck,
                        heartbeat.sequence,
                        kHeartbeatTimeoutMs, completed_responses)) {

                    std::cout
                        << "HEARTBEAT timeout"
                        << " seq=" << heartbeat.sequence
                        << '\n';

                    reconnect_required = true;
                }
            }
            catch (const ConnectionLost& e) {
                std::cout
                    << "Connection lost during HEARTBEAT"
                    << " seq=" << heartbeat.sequence
                    << " error=" << e.what()
                    << '\n';

                reconnect_required = true;
            }

            if (reconnect_required) {
                close(fd);

                std::cout << "Reconnecting...\n";

                fd = connect_with_retry();

                send_packet(fd, heartbeat);

                std::cout
                    << "Resent HEARTBEAT"
                    << " seq=" << heartbeat.sequence
                    << '\n';

                if (!wait_for_response(
                        fd,
                        protocol::MessageType::HeartbeatAck,
                        heartbeat.sequence,
                        kHeartbeatTimeoutMs, completed_responses)) {

                    throw std::runtime_error(
                        "heartbeat failed after reconnect");
                }
            }

            std::cout
                << "Received HEARTBEAT_ACK"
                << " seq=" << heartbeat.sequence
                << '\n';

            ++heartbeat_sequence;
        }

        close(fd);
    }
    catch (const std::exception& e) {
        std::cerr << "Sender error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}