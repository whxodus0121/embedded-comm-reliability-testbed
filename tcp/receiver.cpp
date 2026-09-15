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

constexpr uint16_t kPort = 5001;
constexpr int kBacklog = 5;

class PeerDisconnected : public std::runtime_error {
public:
    PeerDisconnected()
        : std::runtime_error("peer disconnected")
    {
    }
};

void send_all(int fd, const void* data, std::size_t size)
{
    const auto* buffer = static_cast<const uint8_t*>(data);
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
                std::string("send failed: ") + std::strerror(errno)
            );
        }

        if (sent == 0) {
            throw std::runtime_error(
                "connection closed while sending"
            );
        }

        total_sent += static_cast<std::size_t>(sent);
    }
}

std::vector<uint8_t> recv_exact(int fd, std::size_t size)
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
                std::string("recv failed: ") + std::strerror(errno)
            );
        }

        if (received == 0) {
            if (total_received == 0) {
                throw PeerDisconnected();
            }

            throw std::runtime_error(
                "peer disconnected during packet receive"
            );
        }

        total_received += static_cast<std::size_t>(received);
    }

    return buffer;
}

void send_packet(int fd, const protocol::Packet& packet)
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
    // Header의 Length를 이용해 TCP stream에서 메시지 경계를 구분한다.
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

int create_server_socket()
{
    int server_fd = socket(AF_INET, SOCK_STREAM, 0);

    if (server_fd < 0) {
        throw std::runtime_error(
            std::string("socket failed: ") + std::strerror(errno)
        );
    }

    int enable = 1;

    if (setsockopt(
            server_fd,
            SOL_SOCKET,
            SO_REUSEADDR,
            &enable,
            sizeof(enable)) < 0) {

        close(server_fd);

        throw std::runtime_error(
            std::string("setsockopt failed: ") + std::strerror(errno)
        );
    }

    sockaddr_in address{};

    address.sin_family = AF_INET;
    address.sin_port = htons(kPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);

    if (bind(
            server_fd,
            reinterpret_cast<sockaddr*>(&address),
            sizeof(address)) < 0) {

        close(server_fd);

        throw std::runtime_error(
            std::string("bind failed: ") + std::strerror(errno)
        );
    }

    if (listen(server_fd, kBacklog) < 0) {
        close(server_fd);

        throw std::runtime_error(
            std::string("listen failed: ") + std::strerror(errno)
        );
    }

    return server_fd;
}

} // namespace

int main()
{
    try {
        int server_fd = create_server_socket();

        std::cout
            << "Receiver listening on port "
            << kPort
            << '\n';

        // Reconnect 이후에도 같은 DATA의 중복 처리를 막기 위해 유지한다.
        std::unordered_set<uint32_t> processed_sequences;

        while (true) {
            sockaddr_in client_address{};
            socklen_t client_address_length =
                sizeof(client_address);

            int client_fd = accept(
                server_fd,
                reinterpret_cast<sockaddr*>(&client_address),
                &client_address_length
            );

            if (client_fd < 0) {
                if (errno == EINTR) {
                    continue;
                }

                throw std::runtime_error(
                    std::string("accept failed: ") + std::strerror(errno)
                );
            }

            std::cout << "Sender connected\n";

            while (true) {
                try {
                    protocol::Packet packet =
                        receive_packet(client_fd);

                    if (packet.type == protocol::MessageType::Data) {
                        bool duplicate =
                            processed_sequences.find(packet.sequence)
                            != processed_sequences.end();

                        if (!duplicate) {
                            std::string message(
                                packet.payload.begin(),
                                packet.payload.end()
                            );

                            std::cout
                                << "Received DATA"
                                << " seq=" << packet.sequence
                                << " payload=" << message
                                << '\n';

                            processed_sequences.insert(
                                packet.sequence
                            );
                        }
                        else {
                            std::cout
                                << "Duplicate DATA"
                                << " seq=" << packet.sequence
                                << " ignored"
                                << '\n';
                        }

                        protocol::Packet ack{
                            protocol::MessageType::Ack,
                            packet.sequence,
                            {}
                        };

                        send_packet(client_fd, ack);

                        std::cout
                            << "Sent ACK"
                            << " seq=" << ack.sequence
                            << '\n';
                    }
                    else if (
                        packet.type == protocol::MessageType::Heartbeat) {

                        std::cout
                            << "Received HEARTBEAT"
                            << " seq=" << packet.sequence
                            << '\n';

                        protocol::Packet heartbeat_ack{
                            protocol::MessageType::HeartbeatAck,
                            packet.sequence,
                            {}
                        };

                        send_packet(
                            client_fd,
                            heartbeat_ack
                        );

                        std::cout
                            << "Sent HEARTBEAT_ACK"
                            << " seq=" << heartbeat_ack.sequence
                            << '\n';
                    }
                    else {
                        throw std::runtime_error(
                            "unexpected packet type"
                        );
                    }
                }
                catch (const PeerDisconnected&) {
                    std::cout << "Sender disconnected\n";
                    break;
                }
            }

            close(client_fd);

            std::cout << "Waiting for connection\n";
        }

        close(server_fd);
    }
    catch (const std::exception& e) {
        std::cerr
            << "Receiver error: "
            << e.what()
            << '\n';

        return 1;
    }

    return 0;
}
