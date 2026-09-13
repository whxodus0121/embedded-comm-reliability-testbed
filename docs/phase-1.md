# Phase 1 - TCP Binary Protocol Communication

## 1. 목표

C++ TCP Socket을 이용해 Sender와 Receiver 간 기본 통신을 구현하고, TCP의 Byte Stream 특성을 고려한 Binary Protocol과 Length 기반 Framing을 설계한다.

단순 문자열 송수신에서 끝나는 것이 아니라 다음 흐름을 직접 구현하고 검증하는 것을 목표로 했다.

```text
Sender
  ↓
Packet 생성
  ↓
Serialization
  ↓
Binary Wire Format
  ↓
TCP Byte Stream
  ↓
Receiver
  ↓
Header 수신
  ↓
Payload Length 확인
  ↓
Payload 수신
  ↓
Deserialization / CRC 검증
  ↓
DATA 처리
  ↓
ACK 응답
```

Phase 1에서 구현한 주요 기능은 다음과 같다.

- TCP Client / Server Socket 통신
- Partial Send / Receive 처리
- Binary Packet Format 설계
- Serialization / Deserialization
- Network Byte Order 변환
- Length 기반 TCP Framing
- Sequence Number 기반 DATA / ACK 대응
- CRC32 기반 메시지 검증

---

## 2. 배경 개념

### 2.1 TCP Socket

Linux에서는 Socket도 File Descriptor로 관리된다.

Sender는 TCP Client 역할을 수행하며 다음 흐름으로 Receiver에 연결한다.

```text
socket()
  ↓
connect()
  ↓
send() / recv()
```

Receiver는 TCP Server 역할을 수행한다.

```text
socket()
  ↓
bind()
  ↓
listen()
  ↓
accept()
  ↓
recv() / send()
```

이번 프로젝트에서는 다음 주소를 사용했다.

```text
Receiver IP   : 127.0.0.1
Receiver Port : 5000
```

Phase 1에서는 네트워크 자체보다 Protocol과 Socket 동작에 집중하기 위해 동일 Host의 Loopback Interface에서 통신을 검증했다.

### 2.2 TCP는 Byte Stream이다

초기 구현에서는 Sender가 다음 문자열을 전송했다.

```text
hello
```

Receiver는 문자열 길이를 미리 알고 있었기 때문에 단순히 5 byte를 수신했다.

```text
Sender
send("hello", 5)

Receiver
recv_exact(5)
```

하지만 실제 Protocol에서는 Receiver가 앞으로 들어올 메시지의 크기를 미리 알 수 없다.

또한 TCP는 Application Message 단위를 보존하지 않는 Byte Stream 방식이다.

Sender가 한 번의 `send()`로 데이터를 전달했다고 해서 Receiver가 한 번의 `recv()`로 동일한 크기를 받는다고 보장할 수 없다.

예를 들어:

```text
Sender

send("hello")
```

라고 하더라도 Receiver에서는 다음처럼 나뉘어 들어올 수 있다.

```text
recv() → "he"
recv() → "llo"
```

따라서 메시지의 경계를 Application Protocol에서 직접 정의할 필요가 있다.

### 2.3 Framing

이번 프로젝트에서는 Header에 `Payload Length`를 포함하는 방식으로 메시지 경계를 구분했다.

```text
Header 16 byte
    ↓
Payload Length 확인
    ↓
Payload Length만큼 추가 수신
```

이를 위해 Receiver는 먼저 고정 크기 Header를 읽는다.

```text
recv_exact(16)
```

Header에서 Payload Length를 확인한 뒤:

```text
recv_exact(payload_length)
```

를 수행한다.

이 방식으로 TCP Byte Stream 위에서 하나의 Application Message 단위를 복원할 수 있다.

### 2.4 Network Byte Order

멀티바이트 정수의 메모리 표현은 시스템의 Endianness에 따라 달라질 수 있다.

Protocol에서 동일한 Binary Format을 사용하기 위해 정수 필드는 Network Byte Order로 변환한다.

```text
Host Value
   ↓
htons() / htonl()
   ↓
Network Byte Order
   ↓
TCP
   ↓
ntohs() / ntohl()
   ↓
Host Value
```

이번 Protocol에서는 다음 필드에 Byte Order 변환을 적용했다.

- Magic
- Sequence
- Payload Length
- CRC32

### 2.5 CRC32

Packet에는 CRC32 Field를 추가했다.

Sender는 Packet을 전송하기 전에 Protocol Header 일부와 Payload를 기준으로 CRC를 계산한다.

Receiver는 동일한 데이터로 CRC를 다시 계산한 뒤 Header의 CRC 값과 비교한다.

```text
Sender

Packet Data
   ↓
CRC32 계산
   ↓
CRC 포함 후 전송

Receiver

Packet 수신
   ↓
CRC32 재계산
   ↓
Received CRC와 비교
   ↓
일치   → Packet 처리
불일치 → Packet 거부
```

TCP 자체에도 오류 검출 기능이 존재하므로 Application CRC를 "TCP가 손상을 검출하지 못하기 때문에 필요하다"고 정의하지 않았다.

이번 프로젝트에서는 Binary Protocol 수준의 데이터 검증과 이후 Fault Injector를 통한 의도적 데이터 손상 검출을 목적으로 사용한다.

---

## 3. 왜 필요한가

처음 구현한 TCP 통신은 다음처럼 단순했다.

```text
Sender
  ↓
"hello"
  ↓
Receiver
  ↓
"ack"
```

이 구조에서는 Sender와 Receiver가 메시지 크기를 미리 알고 있다는 전제가 필요했다.

하지만 실제 통신에서는 다음 문제를 해결해야 한다.

```text
Receiver는 메시지가 어디서 끝나는지 어떻게 아는가?

send()한 크기와 recv()한 크기가 다르면 어떻게 하는가?

32bit 정수 값을 서로 동일하게 해석하려면 어떻게 하는가?

수신한 Binary Message가 Protocol 규칙에 맞는지 어떻게 검증하는가?

여러 DATA를 전송할 경우 ACK가 어떤 DATA에 대한 응답인지 어떻게 구분하는가?
```

이를 해결하기 위해 문자열 송수신 구조를 Binary Protocol 기반으로 확장했다.

```text
고정 길이 문자열 통신
        ↓
TCP Byte Stream 특성 확인
        ↓
Header 설계
        ↓
Length 기반 Framing
        ↓
Serialization / Deserialization
        ↓
Network Byte Order
        ↓
CRC 검증
        ↓
Sequence 기반 DATA / ACK
```

---

## 4. 시스템 구조

Phase 1의 전체 구조는 다음과 같다.

```text
tcp_sender
    │
    │ Packet
    ▼
encode_packet()
    │
    │ Binary Buffer
    ▼
send_all()
    │
    ▼
Linux TCP Stack
    │
    ▼
127.0.0.1:5000
    │
    ▼
Linux TCP Stack
    │
    ▼
recv_exact()
    │
    ▼
tcp_receiver
    │
    ├── Header 16 byte
    │
    ├── Payload Length 확인
    │
    ├── Payload 수신
    │
    └── decode_packet()
             │
             ├── Magic 검증
             ├── Version 검증
             ├── Length 검증
             └── CRC 검증
```

프로젝트 구조:

```text
embedded-comm-test/
├── CMakeLists.txt
├── protocol/
│   ├── packet.hpp
│   ├── codec.hpp
│   ├── codec.cpp
│   ├── crc32.hpp
│   └── crc32.cpp
└── tcp/
    ├── sender.cpp
    └── receiver.cpp
```

`protocol/`은 Transport와 분리하여 Binary Message의 정의와 Encoding / Decoding을 담당한다.

`tcp/`에서는 Socket 연결과 실제 Byte Stream 송수신을 담당한다.

---

## 5. 구현 과정

### 5.1 기본 TCP Sender / Receiver

먼저 Protocol 없이 TCP 통신 경로부터 구현했다.

Receiver:

```text
socket()
  ↓
bind()
  ↓
listen()
  ↓
accept()
  ↓
recv()
  ↓
send()
```

Sender:

```text
socket()
  ↓
connect()
  ↓
send()
  ↓
recv()
```

초기 검증에서는 다음 통신을 수행했다.

```text
Sender → "hello"
Receiver → "ack"
```

이를 통해 TCP Client / Server Socket 연결과 양방향 통신이 정상적으로 동작하는 것을 먼저 확인했다.

### 5.2 Partial Send / Receive 처리

TCP는 요청한 전체 크기를 한 번의 `send()` 또는 `recv()`에서 처리한다고 보장하지 않는다.

따라서 전체 데이터를 처리할 때까지 반복하는 `send_all()`과 `recv_exact()`을 구현했다.

```text
send_all()

전송해야 할 크기 = N

send()
  ↓
일부 byte 처리
  ↓
남은 크기 다시 send()
  ↓
N byte 완료
```

수신도 같은 방식으로 처리한다.

```text
recv_exact(N)

recv()
  ↓
현재까지 받은 byte 확인
  ↓
N보다 작으면 다시 recv()
  ↓
N byte 완료
```

### 5.3 Binary Packet Format

Protocol의 Wire Format은 다음과 같이 정의했다.

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 byte | Magic |
| 2 | 1 byte | Version |
| 3 | 1 byte | Type |
| 4 | 4 byte | Sequence |
| 8 | 4 byte | Payload Length |
| 12 | 4 byte | CRC32 |
| 16 | N byte | Payload |

고정 Header 크기는 16 byte이다.

```text
+--------+---------+------+----------+--------+-------+
| Magic  | Version | Type | Sequence | Length | CRC32 |
| 2B     | 1B      | 1B   | 4B       | 4B     | 4B    |
+--------+---------+------+----------+--------+-------+
| Payload N bytes                                    |
+----------------------------------------------------+
```

기본 Protocol 값:

```text
Magic       : 0xAA55
Version     : 1
Max Payload : 1024 bytes
```

Message Type은 다음과 같이 정의했다.

```text
DATA          = 1
ACK           = 2
HEARTBEAT     = 3
HEARTBEAT_ACK = 4
```

현재 Phase에서는 DATA와 ACK를 사용하고 Heartbeat 관련 Type은 이후 Phase에서 사용한다.

### 5.4 Packet과 Wire Format 분리

Application 내부에서는 다음과 같은 Packet 구조를 사용한다.

```cpp
struct Packet {
    MessageType type;
    uint32_t sequence;
    std::vector<uint8_t> payload;
};
```

이 구조체 자체를 Network로 직접 전송하지 않는다.

C++ 구조체를 그대로 전송하면 다음 요소에 영향을 받을 수 있기 때문이다.

```text
Padding
Alignment
Endianness
Compiler / Architecture 차이
```

따라서 다음 변환 단계를 명시적으로 구현했다.

```text
Packet
  ↓
encode_packet()
  ↓
정해진 Wire Format
  ↓
Network

Network
  ↓
Binary Buffer
  ↓
decode_packet()
  ↓
Packet
```

### 5.5 Serialization

`encode_packet()`은 Packet의 각 Field를 정의한 순서대로 Byte Buffer에 기록한다.

멀티바이트 정수는 Network Byte Order로 변환한다.

```text
Magic
  ↓ htons()

Sequence
  ↓ htonl()

Payload Length
  ↓ htonl()

CRC32
  ↓ htonl()
```

이후 Payload를 Header 뒤에 연결하여 최종 Binary Buffer를 생성한다.

### 5.6 Length 기반 TCP Framing

Receiver에서는 먼저 Header만 읽는다.

```cpp
recv_exact(fd, protocol::kHeaderSize);
```

Header의 `Payload Length`를 읽은 뒤 해당 크기만큼 Payload를 수신한다.

```text
TCP Stream

[ 16 byte Header ][ Payload ... ]
        │
        ▼
Payload Length
        │
        ▼
recv_exact(payload_length)
```

이를 통해 TCP 자체에는 존재하지 않는 Application Message Boundary를 Protocol에서 구성했다.

### 5.7 Sequence 기반 ACK

DATA Packet에 Sequence Number를 포함했다.

```text
DATA seq=1
```

Receiver는 DATA를 처리한 뒤 동일한 Sequence를 사용하여 ACK를 반환한다.

```text
DATA seq=1
    ↓
Receiver
    ↓
ACK seq=1
```

Sender는 수신한 ACK의 Sequence와 자신이 전송한 DATA Sequence를 비교한다.

```text
ack.sequence == data_packet.sequence
```

이를 통해 어떤 DATA Message에 대한 ACK인지 구분할 수 있도록 했다.

현재 Phase에서는 하나의 DATA만 전송하지만, 이후 Timeout / Retry를 구현할 때 Sequence를 재사용한다.

### 5.8 CRC32 검증

Sender는 CRC Field를 제외한 Protocol 정보와 Payload를 기준으로 CRC32를 계산한다.

```text
Magic
Version
Type
Sequence
Payload Length
Payload
    ↓
CRC32
```

Receiver는 Packet을 Decode할 때 동일한 방식으로 CRC를 다시 계산한다.

두 값이 다르면 Packet을 처리하지 않고 오류로 종료한다.

```text
received_crc != calculated_crc

→ CRC mismatch
```

---

## 6. 핵심 코드

### TCP Stream 수신

```cpp
std::vector<uint8_t> recv_exact(
    int fd,
    std::size_t size
)
{
    std::vector<uint8_t> buffer(size);
    std::size_t total_received = 0;

    while (total_received < size) {
        ssize_t received = recv(
            fd,
            buffer.data() + total_received,
            size - total_received,
            0
        );

        if (received <= 0) {
            // error / disconnect 처리
        }

        total_received +=
            static_cast<std::size_t>(received);
    }

    return buffer;
}
```

### Length 기반 Framing

```cpp
std::vector<uint8_t> header =
    recv_exact(fd, protocol::kHeaderSize);

uint32_t payload_length =
    protocol::get_payload_length(
        header.data(),
        header.size()
    );

std::vector<uint8_t> payload =
    recv_exact(fd, payload_length);
```

### Packet Decode

```text
Header 수신
    ↓
Magic / Version 확인
    ↓
Sequence 확인
    ↓
Payload Length 확인
    ↓
Payload 수신
    ↓
CRC32 재계산
    ↓
Packet 복원
```

---

## 7. 실행 및 검증

### 7.1 Build

```bash
cmake -S . -B build
cmake --build build
```

생성되는 실행파일:

```text
build/tcp_sender
build/tcp_receiver
```

### 7.2 정상 DATA / ACK 통신

Receiver를 먼저 실행했다.

```bash
./build/tcp_receiver
```

Sender 실행:

```bash
./build/tcp_sender
```

Receiver 결과:

```text
Receiver listening on port 5000
Sender connected
Received DATA seq=1 payload=hello embedded
Sent ACK seq=1
```

Sender 결과:

```text
Connected to receiver
Sent DATA seq=1 payload=hello embedded
Received ACK seq=1
```

이를 통해 다음 경로가 정상적으로 동작하는 것을 확인했다.

```text
DATA Packet
  ↓
Serialization
  ↓
TCP
  ↓
Framing
  ↓
Deserialization
  ↓
CRC Validation
  ↓
ACK Packet
```

### 7.3 CRC 오류 검증

CRC 검증이 실제로 동작하는지 확인하기 위해 테스트 과정에서 `encode_packet()`이 완료된 이후 첫 번째 Payload Byte를 임시로 변경했다.

```text
정상 Packet 생성
    ↓
CRC 계산 완료
    ↓
Payload 1 byte 변조
    ↓
전송
```

Receiver 결과:

```text
Receiver listening on port 5000
Sender connected
Receiver error: CRC mismatch
```

Sender는 Receiver가 손상된 Packet을 거부하고 연결을 종료했기 때문에 다음 결과를 확인했다.

```text
Connected to receiver
Sent DATA seq=1 payload=hello embedded
Sender error: peer disconnected
```

검증 후 Payload 변조 코드는 제거하고 다시 정상 통신을 확인했다.

테스트 결과:

| Scenario | 결과 | 판정 |
|---|---|---|
| Normal DATA → ACK | `DATA seq=1` 수신 후 `ACK seq=1` 응답 | PASS |
| Payload Corruption | CRC 불일치 검출 후 Packet 거부 | PASS |
| Normal Regression | 변조 코드 제거 후 정상 DATA / ACK 재확인 | PASS |

---

## 8. 배운 점

처음에는 TCP 통신을 단순히 다음과 같이 생각했다.

```text
Sender send()
    ↓
Receiver recv()
```

하지만 직접 구현하면서 TCP는 Application Message가 아닌 Byte Stream을 제공하기 때문에 `send()`와 `recv()` 호출 자체가 메시지 경계를 의미하지 않는다는 점을 확인했다.

따라서 실제 Application Protocol에서는 다음 과정이 필요했다.

```text
TCP Byte Stream
    ↓
고정 크기 Header
    ↓
Payload Length
    ↓
Message Framing
```

또한 C++의 Packet 구조체와 실제 Network Wire Format을 분리하고 직접 Serialization / Deserialization을 구현하면서, Network Protocol에서 Endianness와 데이터 표현 방식을 명확하게 정의해야 하는 이유를 확인했다.

최종적인 통신 흐름은 다음과 같다.

```text
Application Packet
    ↓
Serialization
    ↓
Network Byte Order
    ↓
TCP Byte Stream
    ↓
Length-based Framing
    ↓
Deserialization
    ↓
CRC Validation
    ↓
Application Packet
```

CRC 오류 테스트에서는 정상적으로 Encoding된 Packet의 Payload를 의도적으로 변경하고 Receiver가 이를 거부하는 것을 확인했다.

Phase 1에서는 하나의 DATA와 ACK를 정상적으로 교환하는 수준까지 구현했다.

다음 Phase에서는 현재 구조에 Application-level Reliability를 추가한다.

```text
Sequence
  ↓
Application ACK
  ↓
Timeout
  ↓
Retry
  ↓
Heartbeat
  ↓
Disconnect Detection
  ↓
Reconnect
```

이를 통해 단순 통신 성공뿐 아니라 통신 장애가 발생했을 때 Application이 어떻게 상태를 판단하고 복구할 것인지 구현한다.
