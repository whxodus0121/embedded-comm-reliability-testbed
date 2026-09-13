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
#include <vector>

namespace {

constexpr const char* kServerIp = "127.0.0.1";
constexpr uint16_t kServerPort = 5000;

void send_all(int fd, const void* data, std::size_t size)
{
    const auto* buffer =
        static_cast<const uint8_t*>(data);

    std::size_t total_sent = 0;

    // send()가 일부 바이트만 처리할 수 있으므로 끝까지 반복한다.
    while (total_sent < size) {
        ssize_t sent = send(
            fd,
            buffer + total_sent,
            size - total_sent,
            0
        );

        if (sent < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error(
                std::string("send failed: ")
                + std::strerror(errno)
            );
        }

        if (sent == 0) {
            throw std::runtime_error(
                "connection closed while sending"
            );
        }

        total_sent +=
            static_cast<std::size_t>(sent);
    }
}

std::vector<uint8_t> recv_exact(
    int fd,
    std::size_t size
)
{
    std::vector<uint8_t> buffer(size);
    std::size_t total_received = 0;

    // TCP는 byte stream이므로 필요한 크기가 모일 때까지 반복 수신한다.
    while (total_received < size) {
        ssize_t received = recv(
            fd,
            buffer.data() + total_received,
            size - total_received,
            0
        );

        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }

            throw std::runtime_error(
                std::string("recv failed: ")
                + std::strerror(errno)
            );
        }

        if (received == 0) {
            throw std::runtime_error(
                "peer disconnected"
            );
        }

        total_received +=
            static_cast<std::size_t>(received);
    }

    return buffer;
}

void send_packet(
    int fd,
    const protocol::Packet& packet
)
{
    std::vector<uint8_t> encoded =
        protocol::encode_packet(packet);

    send_all(
        fd,
        encoded.data(),
        encoded.size()
    );
}

protocol::Packet receive_packet(int fd)
{
    // 고정 크기 Header를 먼저 읽고 Length를 기준으로 Payload 경계를 결정한다.
    std::vector<uint8_t> header =
        recv_exact(fd, protocol::kHeaderSize);

    uint32_t payload_length =
        protocol::get_payload_length(
            header.data(),
            header.size()
        );

    std::vector<uint8_t> payload =
        recv_exact(fd, payload_length);

    return protocol::decode_packet(
        header.data(),
        header.size(),
        payload.data(),
        payload.size()
    );
}

int connect_to_receiver()
{
    int fd = socket(
        AF_INET,
        SOCK_STREAM,
        0
    );

    if (fd < 0) {
        throw std::runtime_error(
            std::string("socket failed: ")
            + std::strerror(errno)
        );
    }

    sockaddr_in server_address{};

    server_address.sin_family = AF_INET;
    server_address.sin_port =
        htons(kServerPort);

    if (inet_pton(
            AF_INET,
            kServerIp,
            &server_address.sin_addr) != 1) {

        close(fd);

        throw std::runtime_error(
            "invalid server IP address"
        );
    }

    if (connect(
            fd,
            reinterpret_cast<sockaddr*>(
                &server_address
            ),
            sizeof(server_address)) < 0) {

        close(fd);

        throw std::runtime_error(
            std::string("connect failed: ")
            + std::strerror(errno)
        );
    }

    return fd;
}

} // namespace

int main()
{
    try {
        int fd = connect_to_receiver();

        std::cout
            << "Connected to receiver\n";

        std::string message =
            "hello embedded";

        protocol::Packet data_packet{
            protocol::MessageType::Data,
            1,
            std::vector<uint8_t>(
                message.begin(),
                message.end()
            )
        };

        send_packet(fd, data_packet);

        std::cout
            << "Sent DATA"
            << " seq=" << data_packet.sequence
            << " payload=" << message
            << '\n';

        protocol::Packet ack =
            receive_packet(fd);

        if (ack.type
            != protocol::MessageType::Ack) {

            throw std::runtime_error(
                "expected ACK packet"
            );
        }

        // ACK의 sequence로 어떤 DATA에 대한 응답인지 확인한다.
        if (ack.sequence
            != data_packet.sequence) {

            throw std::runtime_error(
                "ACK sequence mismatch"
            );
        }

        std::cout
            << "Received ACK"
            << " seq=" << ack.sequence
            << '\n';

        close(fd);
    }
    catch (const std::exception& e) {
        std::cerr
            << "Sender error: "
            << e.what()
            << '\n';

        return 1;
    }

    return 0;
}
