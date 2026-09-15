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
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr uint16_t kListenPort = 5000;
constexpr const char* kReceiverIp = "127.0.0.1";
constexpr uint16_t kReceiverPort = 5001;
constexpr int kBacklog = 5;
constexpr int kAckDelayMs = 700;

enum class FaultMode {
    None,
    DropAck,
    DelayAck,
    CorruptData,
    Disconnect
};

class PeerDisconnected : public std::runtime_error {
public:
    explicit PeerDisconnected(const std::string& message)
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

            throw std::runtime_error(
                std::string("send failed: ") + std::strerror(errno));
        }

        if (sent == 0) {
            throw PeerDisconnected("connection closed while sending");
        }

        total_sent += static_cast<std::size_t>(sent);
    }
}

std::vector<uint8_t> recv_exact(int fd, std::size_t size)
{
    std::vector<uint8_t> buffer(size);
    std::size_t total_received = 0;

    while (total_received < size) {
        ssize_t received = recv(
            fd, buffer.data() + total_received, size - total_received, 0);

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error(
                std::string("recv failed: ") + std::strerror(errno));
        }

        if (received == 0) {
            throw PeerDisconnected("peer disconnected");
        }

        total_received += static_cast<std::size_t>(received);
    }

    return buffer;
}

void send_packet(int fd, const protocol::Packet& packet)
{
    std::vector<uint8_t> encoded = protocol::encode_packet(packet);
    send_all(fd, encoded.data(), encoded.size());
}

void send_corrupted_packet(int fd, const protocol::Packet& packet)
{
    std::vector<uint8_t> encoded = protocol::encode_packet(packet);

    if (encoded.size() <= protocol::kHeaderSize) {
        throw std::runtime_error("cannot corrupt packet without payload");
    }

    encoded[protocol::kHeaderSize] ^= 0xFF;
    send_all(fd, encoded.data(), encoded.size());
}

protocol::Packet receive_packet(int fd)
{
    std::vector<uint8_t> header = recv_exact(fd, protocol::kHeaderSize);

    uint32_t payload_length =
        protocol::get_payload_length(header.data(), header.size());

    std::vector<uint8_t> payload = recv_exact(fd, payload_length);

    return protocol::decode_packet(
        header.data(), header.size(), payload.data(), payload.size());
}

const char* message_type_name(protocol::MessageType type)
{
    switch (type) {
    case protocol::MessageType::Data:
        return "DATA";
    case protocol::MessageType::Ack:
        return "ACK";
    case protocol::MessageType::Heartbeat:
        return "HEARTBEAT";
    case protocol::MessageType::HeartbeatAck:
        return "HEARTBEAT_ACK";
    default:
        return "UNKNOWN";
    }
}

const char* fault_mode_name(FaultMode mode)
{
    switch (mode) {
    case FaultMode::None:
        return "none";
    case FaultMode::DropAck:
        return "drop-ack";
    case FaultMode::DelayAck:
        return "delay-ack";
    case FaultMode::CorruptData:
        return "corrupt-data";
    case FaultMode::Disconnect:
        return "disconnect";
    default:
        return "unknown";
    }
}

FaultMode parse_fault_mode(int argc, char* argv[])
{
    if (argc < 2) {
        return FaultMode::None;
    }

    std::string mode = argv[1];

    if (mode == "none") {
        return FaultMode::None;
    }

    if (mode == "drop-ack") {
        return FaultMode::DropAck;
    }

    if (mode == "delay-ack") {
        return FaultMode::DelayAck;
    }

    if (mode == "corrupt-data") {
        return FaultMode::CorruptData;
    }

    if (mode == "disconnect") {
        return FaultMode::Disconnect;
    }

    throw std::runtime_error("unknown fault mode: " + mode);
}

int create_listen_socket()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        throw std::runtime_error(
            std::string("socket failed: ") + std::strerror(errno));
    }

    int enable = 1;

    if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) < 0) {
        close(fd);

        throw std::runtime_error(
            std::string("setsockopt failed: ") + std::strerror(errno));
    }

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(kListenPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) < 0) {
        close(fd);

        throw std::runtime_error(
            std::string("bind failed: ") + std::strerror(errno));
    }

    if (listen(fd, kBacklog) < 0) {
        close(fd);

        throw std::runtime_error(
            std::string("listen failed: ") + std::strerror(errno));
    }

    return fd;
}

int connect_to_receiver()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);

    if (fd < 0) {
        throw std::runtime_error(
            std::string("socket failed: ") + std::strerror(errno));
    }

    sockaddr_in receiver_address{};
    receiver_address.sin_family = AF_INET;
    receiver_address.sin_port = htons(kReceiverPort);

    if (inet_pton(AF_INET, kReceiverIp, &receiver_address.sin_addr) != 1) {
        close(fd);
        throw std::runtime_error("invalid receiver IP address");
    }

    if (connect(
            fd,
            reinterpret_cast<sockaddr*>(&receiver_address),
            sizeof(receiver_address)) < 0) {

        close(fd);

        throw std::runtime_error(
            std::string("connect to receiver failed: ") + std::strerror(errno));
    }

    return fd;
}

void relay_connection(
    int sender_fd,
    int receiver_fd,
    FaultMode fault_mode,
    bool& fault_applied)
{
    pollfd fds[2]{};

    fds[0].fd = sender_fd;
    fds[0].events = POLLIN;

    fds[1].fd = receiver_fd;
    fds[1].events = POLLIN;

    while (true) {
        int result;

        do {
            result = poll(fds, 2, -1);
        } while (result < 0 && errno == EINTR);

        if (result < 0) {
            throw std::runtime_error(
                std::string("poll failed: ") + std::strerror(errno));
        }

        if (fds[0].revents & POLLIN) {
            protocol::Packet packet = receive_packet(sender_fd);

            if (packet.type == protocol::MessageType::Data &&
                fault_mode == FaultMode::CorruptData &&
                !fault_applied) {

                std::cout << "[CORRUPT] DATA seq=" << packet.sequence << '\n';

                send_corrupted_packet(receiver_fd, packet);
                fault_applied = true;
                continue;
            }

            if (packet.type == protocol::MessageType::Heartbeat &&
                fault_mode == FaultMode::Disconnect &&
                !fault_applied) {

                std::cout
                    << "[DISCONNECT] HEARTBEAT"
                    << " seq=" << packet.sequence
                    << '\n';

                fault_applied = true;

                shutdown(sender_fd, SHUT_RDWR);
                shutdown(receiver_fd, SHUT_RDWR);

                throw PeerDisconnected("forced disconnect injected");
            }

            std::cout
                << "[Sender -> Receiver] "
                << message_type_name(packet.type)
                << " seq=" << packet.sequence
                << '\n';

            send_packet(receiver_fd, packet);
        }

        if (fds[1].revents & POLLIN) {
            protocol::Packet packet = receive_packet(receiver_fd);

            if (packet.type == protocol::MessageType::Ack && !fault_applied) {
                if (fault_mode == FaultMode::DropAck) {
                    std::cout << "[DROP] ACK seq=" << packet.sequence << '\n';
                    fault_applied = true;
                    continue;
                }

                if (fault_mode == FaultMode::DelayAck) {
                    std::cout
                        << "[DELAY " << kAckDelayMs << "ms] ACK"
                        << " seq=" << packet.sequence
                        << '\n';

                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(kAckDelayMs));

                    fault_applied = true;
                }
            }

            std::cout
                << "[Receiver -> Sender] "
                << message_type_name(packet.type)
                << " seq=" << packet.sequence
                << '\n';

            send_packet(sender_fd, packet);
        }

        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw PeerDisconnected("sender connection closed");
        }

        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL)) {
            throw PeerDisconnected("receiver connection closed");
        }
    }
}

} // namespace

int main(int argc, char* argv[])
{
    try {
        FaultMode fault_mode = parse_fault_mode(argc, argv);
        int listen_fd = create_listen_socket();
        bool fault_applied = false;

        std::cout << "Fault Injector listening on port " << kListenPort << '\n';
        std::cout << "Fault mode: " << fault_mode_name(fault_mode) << '\n';

        while (true) {
            sockaddr_in sender_address{};
            socklen_t sender_address_length = sizeof(sender_address);

            int sender_fd = accept(
                listen_fd,
                reinterpret_cast<sockaddr*>(&sender_address),
                &sender_address_length);

            if (sender_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(
                    std::string("accept failed: ") + std::strerror(errno));
            }

            std::cout << "Sender connected to Fault Injector\n";

            int receiver_fd = -1;

            try {
                receiver_fd = connect_to_receiver();

                std::cout << "Fault Injector connected to Receiver\n";

                relay_connection(
                    sender_fd, receiver_fd, fault_mode, fault_applied);
            }
            catch (const PeerDisconnected& e) {
                std::cout << "Proxy connection closed: " << e.what() << '\n';
            }
            catch (const std::exception& e) {
                std::cerr << "Proxy connection error: " << e.what() << '\n';
            }

            close(sender_fd);

            if (receiver_fd >= 0) {
                close(receiver_fd);
            }

            std::cout << "Waiting for next Sender connection\n";
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Fault Injector error: " << e.what() << '\n';
        return 1;
    }

    return 0;
}