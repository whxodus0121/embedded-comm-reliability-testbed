# Phase 4 - UDP Reliability Extension

## 1. 목표

Phase 4의 목표는 기존 Binary Protocol을 UDP 환경에서도 재사용하고, TCP가 Transport Layer에서 제공하던 신뢰성 기능이 없는 UDP에서 Application Layer가 직접 신뢰성을 보완하는 과정을 구현하는 것이다.

주요 목표는 다음과 같다.

- 기존 Binary Protocol을 UDP Datagram 기반으로 재사용
- UDP DATA / ACK 통신 구현
- ACK Timeout 및 Retry 구현
- 동일 Sequence 재전송에 대한 Duplicate Detection 구현
- Sequence 기반 Out-of-order Detection 구현
- TCP와 UDP의 통신 및 신뢰성 처리 차이 비교

---

## 2. 배경 개념

### TCP와 UDP의 데이터 전달 방식

TCP는 Byte Stream 기반 프로토콜이다.

Application에서 여러 번 `send()`를 호출하더라도 Receiver가 동일한 단위로 `recv()`한다는 보장이 없다.

따라서 이전 Phase에서는 Protocol Header의 Payload Length를 기준으로 Header와 Payload를 정확히 읽는 Framing 로직이 필요했다.

반면 UDP는 Datagram 기반 프로토콜이다.

하나의 `sendto()` 호출로 전송한 Datagram은 Receiver의 하나의 `recvfrom()` 단위로 전달된다. 따라서 Application Message 하나를 UDP Datagram 하나에 대응시킬 수 있다.

```text
TCP

send(Packet A)
send(Packet B)

        ↓

Byte Stream

AAAAAAAABBBBBBBB...
```

```text
UDP

sendto(Packet A)
sendto(Packet B)

        ↓

[ Datagram A ]
[ Datagram B ]
```

이 때문에 UDP에서는 TCP에서 사용했던 `recv_exact()` 기반 Framing이 필요하지 않다.

---

### UDP의 신뢰성 특성

UDP는 Datagram 전달에 대해 다음을 보장하지 않는다.

- 전달 보장
- 재전송
- 순서 보장
- 중복 제거

따라서 신뢰성이 필요한 Application이라면 이러한 기능 중 필요한 부분을 직접 구현해야 한다.

이번 프로젝트에서는 다음 구조를 사용했다.

```text
DATA seq=N
    ↓
ACK 대기
    ↓
Timeout 발생
    ↓
동일 seq=N 재전송
    ↓
Receiver Duplicate Detection
```

---

### TCP ACK와 Application ACK의 차이

이 프로젝트의 `MessageType::Ack`는 TCP 자체의 ACK와 다른 Application Layer 메시지이다.

TCP ACK는 TCP Stack이 Byte Stream 전달을 위해 내부적으로 관리한다.

반면 프로젝트의 Application ACK는 다음 의미를 가진다.

```text
DATA seq=1
    ↓
Receiver Application이 메시지 수신 및 처리
    ↓
ACK seq=1
```

TCP에서도 Application 처리 여부를 확인하기 위해 별도의 ACK를 구현했지만, UDP에서는 Transport Layer 자체의 재전송 기능도 없기 때문에 Application ACK / Timeout / Retry가 Datagram 손실 복구에도 직접 사용된다.

---

## 3. 왜 필요한가

단순히 UDP로 데이터를 송수신하는 것만으로는 신뢰성 있는 통신을 구성할 수 없다.

예를 들어 Sender가 다음 호출에 성공했다고 하더라도,

```cpp
sendto(...)
```

이는 Local Socket에 Datagram 전송 요청이 정상적으로 처리되었다는 의미이지, Receiver Application이 해당 데이터를 실제로 수신했다는 의미는 아니다.

Datagram이 손실되면 UDP는 자동으로 재전송하지 않는다.

따라서 Application에서 ACK를 기다리고 일정 시간 동안 ACK가 도착하지 않으면 동일 Sequence의 DATA를 다시 전송해야 한다.

하지만 ACK 자체가 손실될 수도 있다.

```text
Sender                    Receiver

DATA seq=1  ------------>
                          DATA 처리

             ACK seq=1
               X

Timeout

DATA seq=1  ------------>
```

이 경우 Receiver는 동일한 DATA를 두 번 받을 수 있다.

따라서 Retry만 구현하면 부족하며, Sequence를 이용한 Duplicate Detection이 함께 필요하다.

또한 UDP는 Datagram의 순서를 보장하지 않으므로 `seq=1, 3, 2`와 같이 도착하는 경우를 감지할 수 있어야 한다.

---

## 4. 시스템 구조

Phase 4에서는 TCP Fault Injector와 분리하여 UDP Sender와 Receiver를 직접 연결했다.

```text
┌─────────────────┐             ┌─────────────────┐
│   UDP Sender    │             │  UDP Receiver   │
│                 │             │                 │
│ Binary Protocol │   Datagram  │ Binary Protocol │
│ ACK Timeout     │ ----------> │ Sequence Check  │
│ Retry           │ <---------- │ ACK             │
└─────────────────┘             └─────────────────┘
                                         │
                                         ├─ Duplicate Detection
                                         └─ Out-of-order Detection
```

UDP Receiver는 `6000`번 Port를 사용한다.

```text
TCP Fault Injector : 5000
TCP Receiver       : 5001
UDP Receiver       : 6000
```

기존 Protocol 구조는 변경하지 않았다.

```text
Offset  Size  Field
0       2     Magic
2       1     Version
3       1     Type
4       4     Sequence
8       4     Payload Length
12      4     CRC32
16      N     Payload
```

Transport Layer만 TCP에서 UDP로 변경하고, 기존 `encode_packet()`과 `decode_packet()`을 그대로 재사용했다.

---

## 5. 구현 과정

### 5.1 UDP 기본 통신

UDP Socket은 다음과 같이 생성했다.

```cpp
int fd = socket(AF_INET, SOCK_DGRAM, 0);
```

TCP의 `SOCK_STREAM`과 달리 UDP는 `SOCK_DGRAM`을 사용한다.

Receiver는 Port 6000에 Socket을 Bind한다.

```cpp
bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
```

UDP에는 TCP Connection 개념이 없으므로 `listen()`과 `accept()` 과정이 없다.

```text
TCP Receiver

socket()
→ bind()
→ listen()
→ accept()
→ recv()
```

```text
UDP Receiver

socket()
→ bind()
→ recvfrom()
```

Sender 역시 TCP의 `connect()` 대신 Destination 주소를 지정하여 `sendto()`를 사용했다.

---

### 5.2 Datagram 단위 Protocol 처리

Sender는 기존 Binary Protocol Encoder를 사용한다.

```cpp
std::vector<uint8_t> encoded = protocol::encode_packet(packet);
sendto(fd, encoded.data(), encoded.size(), 0, reinterpret_cast<const sockaddr*>(&destination), sizeof(destination));
```

Receiver에서는 Datagram 하나를 Buffer에 수신한다.

```cpp
ssize_t received = recvfrom(fd, buffer.data(), buffer.size(), 0, reinterpret_cast<sockaddr*>(&sender_address), &sender_address_length);
```

TCP와 달리 Datagram 경계가 유지되므로 Header와 Payload를 별도의 `recv_exact()` 호출로 나누지 않는다.

하지만 Datagram 자체가 Protocol 규격과 일치하는지는 검증한다.

```cpp
uint32_t payload_length = protocol::get_payload_length(buffer.data(), protocol::kHeaderSize);
std::size_t expected_size = protocol::kHeaderSize + payload_length;
```

실제 Datagram 크기와 Header에 기록된 Payload Length가 일치하지 않으면 오류로 처리한다.

이후 기존 Decoder를 그대로 사용한다.

```cpp
return protocol::decode_packet(buffer.data(), protocol::kHeaderSize, buffer.data() + protocol::kHeaderSize, payload_length);
```

따라서 Magic, Version, Length, CRC 등의 Protocol 검증 로직을 TCP와 UDP에서 공유할 수 있다.

---

### 5.3 ACK Timeout

UDP Sender는 DATA 전송 후 `poll()`을 이용해 ACK를 기다린다.

```cpp
pollfd pfd{};
pfd.fd = fd;
pfd.events = POLLIN;

int result = poll(&pfd, 1, kAckTimeoutMs);
```

ACK Timeout은 1000ms로 설정했다.

```cpp
constexpr int kAckTimeoutMs = 1000;
```

`poll()` 결과가 0이면 Timeout으로 판단한다.

```cpp
if (result == 0) {
    return false;
}
```

ACK가 수신되면 Type과 Sequence를 검증한다.

```cpp
if (ack.type != protocol::MessageType::Ack) {
    throw std::runtime_error("expected ACK packet");
}

if (ack.sequence != expected_sequence) {
    throw std::runtime_error("ACK sequence mismatch");
}
```

---

### 5.4 Retry

ACK Timeout이 발생하면 동일한 DATA를 다시 전송한다.

```cpp
constexpr int kMaxRetries = 3;
```

초기 전송 1회와 추가 Retry 최대 3회를 허용하므로 총 전송 시도 횟수는 최대 4회이다.

```cpp
for (int attempt = 1; attempt <= kMaxRetries + 1; ++attempt) {
    send_packet(fd, data_packet, receiver_address);

    if (wait_for_ack(fd, sequence)) {
        return;
    }
}
```

Retry 시 새로운 Sequence를 생성하지 않는다.

```text
1차 전송  DATA seq=1
Retry     DATA seq=1
Retry     DATA seq=1
```

동일한 논리 메시지에 동일한 Sequence를 유지해야 Receiver가 중복 여부를 판단할 수 있다.

---

### 5.5 Duplicate Detection

ACK Loss 상황을 재현하기 위해 Receiver에 `drop-first-ack` 테스트 모드를 추가했다.

```bash
./build/udp_receiver drop-first-ack
```

첫 DATA는 정상 처리하지만 첫 ACK는 의도적으로 전송하지 않는다.

```text
Received DATA seq=1
ACK intentionally dropped seq=1
```

Sender에서는 ACK Timeout이 발생하고 동일한 `seq=1` DATA를 다시 전송한다.

Receiver는 처리한 Sequence를 저장한다.

```cpp
std::unordered_set<uint32_t> processed_sequences;
```

이미 존재하는 Sequence라면 실제 DATA 처리를 반복하지 않는다.

```cpp
bool duplicate = processed_sequences.find(packet.sequence) != processed_sequences.end();
```

결과:

```text
Duplicate DATA seq=1 ignored
```

하지만 ACK는 다시 전송한다.

이를 통해 ACK 손실 상황에서도 중복 처리를 방지하면서 Sender가 정상적으로 복구할 수 있다.

---

### 5.6 Out-of-order Detection

UDP는 Datagram 순서를 보장하지 않는다.

localhost 환경에서 실제 네트워크가 임의로 Datagram 순서를 변경하기를 기다리는 방식은 테스트 재현성이 낮기 때문에 Sender에서 의도적으로 다음 순서로 DATA를 전송했다.

```text
seq=1
seq=3
seq=2
```

실행 모드는 다음과 같다.

```bash
./build/udp_receiver out-of-order
./build/udp_sender out-of-order
```

Receiver는 다음 Sequence를 추적한다.

```cpp
uint32_t expected_sequence = 1;
```

예상 Sequence보다 큰 값이 먼저 도착하면 순서 이상으로 판단한다.

```cpp
if (packet.sequence > expected_sequence) {
    std::cout << "Out-of-order DATA seq=" << packet.sequence << " expected=" << expected_sequence << '\n';
}
```

예를 들어 `seq=1` 이후 `seq=3`이 도착하면 다음과 같이 감지된다.

```text
Out-of-order DATA seq=3 expected=2
```

이미 수신한 Sequence Set을 이용해 누락되었던 Sequence가 이후 도착했는지도 추적한다.

```cpp
while (processed_sequences.find(expected_sequence) != processed_sequences.end()) {
    ++expected_sequence;
}
```

`seq=2`가 나중에 도착하면 `1, 2, 3`이 모두 존재하므로 `expected_sequence`는 4까지 진행한다.

이번 구현에서는 Out-of-order를 감지하는 것까지만 수행하며 별도의 Reordering Buffer나 Sliding Window는 구현하지 않았다.

---

## 6. 핵심 코드

### UDP Datagram 전송

```cpp
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
```

### ACK Timeout

```cpp
bool wait_for_ack(int fd, uint32_t expected_sequence)
{
    pollfd pfd{};
    pfd.fd = fd;
    pfd.events = POLLIN;

    int result = poll(&pfd, 1, kAckTimeoutMs);

    if (result == 0) {
        return false;
    }

    protocol::Packet ack = receive_packet(fd);

    if (ack.type != protocol::MessageType::Ack || ack.sequence != expected_sequence) {
        throw std::runtime_error("invalid ACK");
    }

    return true;
}
```

### Retry

```cpp
for (int attempt = 1; attempt <= kMaxRetries + 1; ++attempt) {
    send_packet(fd, data_packet, receiver_address);

    if (wait_for_ack(fd, sequence)) {
        return;
    }

    std::cout << "ACK timeout seq=" << sequence << '\n';
}
```

### Duplicate Detection

```cpp
bool duplicate = processed_sequences.find(packet.sequence) != processed_sequences.end();

if (duplicate) {
    std::cout << "Duplicate DATA seq=" << packet.sequence << " ignored\n";
}
else {
    processed_sequences.insert(packet.sequence);
}
```

### Out-of-order Detection

```cpp
if (packet.sequence > expected_sequence) {
    std::cout << "Out-of-order DATA seq=" << packet.sequence << " expected=" << expected_sequence << '\n';
}

processed_sequences.insert(packet.sequence);

while (processed_sequences.find(expected_sequence) != processed_sequences.end()) {
    ++expected_sequence;
}
```

---

## 7. 실행 및 검증

### 7.1 UDP 기본 통신

Receiver:

```bash
./build/udp_receiver
```

Sender:

```bash
./build/udp_sender
```

결과:

```text
Sender
Sent DATA seq=1 payload=hello udp
Received ACK seq=1

Receiver
Received DATA seq=1 payload=hello udp
Sent ACK seq=1
```

Binary Protocol을 변경하지 않고 UDP Datagram을 통해 DATA와 ACK를 정상적으로 송수신했다.

![UDP Basic Communication](images/phase-4-udp-basic.png)

**PASS**

---

### 7.2 ACK Loss / Timeout / Retry / Duplicate Detection

Receiver:

```bash
./build/udp_receiver drop-first-ack
```

Sender:

```bash
./build/udp_sender
```

Sender 결과:

```text
Sent DATA seq=1 attempt=1 payload=hello udp
ACK timeout seq=1
Sent DATA seq=1 attempt=2 payload=hello udp
Received ACK seq=1
```

Receiver 결과:

```text
Received DATA seq=1 payload=hello udp
ACK intentionally dropped seq=1
Duplicate DATA seq=1 ignored
Sent ACK seq=1
```

첫 ACK를 의도적으로 누락한 결과 Sender에서 Timeout이 발생했고 동일 Sequence의 DATA가 재전송되었다.

Receiver는 재전송된 `seq=1`을 Duplicate로 감지하여 실제 처리는 반복하지 않고 ACK만 다시 전송했다.

![UDP Retry and Duplicate Detection](images/phase-4-udp-retry.png)

**PASS**

---

### 7.3 Out-of-order Detection

Receiver:

```bash
./build/udp_receiver out-of-order
```

Sender:

```bash
./build/udp_sender out-of-order
```

Sender는 테스트 재현을 위해 다음 순서로 DATA를 전송했다.

```text
1 → 3 → 2
```

Receiver 결과:

```text
Received DATA seq=1 payload=message-1
Sent ACK seq=1

Out-of-order DATA seq=3 expected=2
Received DATA seq=3 payload=message-3
Sent ACK seq=3

Received DATA seq=2 payload=message-2
Sent ACK seq=2
```

Receiver가 `seq=2`를 기대하던 상태에서 `seq=3`이 먼저 도착한 것을 감지했다.

이 테스트는 실제 네트워크가 Datagram을 임의로 재정렬한 결과가 아니라, UDP 환경에서 발생할 수 있는 순서 역전을 통제된 입력으로 재현하여 Application의 감지 로직을 검증한 것이다.

![UDP Out-of-order Detection](images/phase-4-udp-out-of-order.png)

**PASS**

---

### 테스트 결과

| 테스트 | 검증 내용 | 결과 |
|---|---|---|
| UDP Basic | Datagram 기반 DATA / ACK 송수신 | PASS |
| ACK Loss | ACK 미수신 시 Timeout 발생 | PASS |
| Retry | 동일 Sequence DATA 재전송 | PASS |
| Duplicate | 중복 DATA 처리 방지 | PASS |
| Recovery | Retry 이후 ACK 수신 및 정상 종료 | PASS |
| Out-of-order | 예상 Sequence보다 큰 DATA 선도착 감지 | PASS |

---

## 8. 배운 점

이번 Phase를 통해 TCP와 UDP의 차이를 단순히 Connection-oriented / Connectionless 수준이 아니라 Application 구현 관점에서 확인할 수 있었다.

TCP에서는 Byte Stream 특성 때문에 Application Protocol의 Message 경계를 직접 복원해야 했다. 반면 UDP는 Datagram 경계가 유지되므로 Packet 하나를 Datagram 하나에 대응시킬 수 있었고 TCP에서 사용한 `recv_exact()` 기반 Framing이 필요하지 않았다.

반대로 신뢰성 측면에서는 UDP가 제공하는 기능이 훨씬 적었다. UDP의 `sendto()`가 성공하더라도 Receiver Application의 수신을 보장하지 않기 때문에 Application ACK와 Timeout을 이용해 전달 여부를 확인해야 했다.

Retry를 추가하는 것만으로는 충분하지 않았다. ACK가 손실되면 Receiver가 이미 처리한 DATA가 다시 전달될 수 있기 때문에 Sequence 기반 Duplicate Detection이 함께 필요했다.

또한 UDP는 순서를 보장하지 않으므로 Sequence는 단순한 ACK 매칭 용도뿐 아니라 Out-of-order Detection에도 사용할 수 있었다.

TCP와 UDP에서 동일한 Binary Protocol과 Sequence 개념을 사용했지만 Transport 특성에 따라 Application에서 해결해야 하는 문제가 달라진다는 점을 확인했다.

현재 구현은 신뢰성 메커니즘을 학습하고 검증하기 위한 최소 구조이며 다음 기능은 범위에서 제외했다.

- Sliding Window
- Selective ACK
- Out-of-order Reordering Buffer
- Congestion Control
- RTT 기반 동적 Timeout
- Persistent Sequence / Session ID
- Multi-client UDP Session 관리

이러한 기능까지 추가하면 별도의 Reliable UDP Protocol 설계 영역으로 범위가 확대되므로 이번 프로젝트에서는 ACK / Timeout / Retry / Duplicate / Out-of-order Detection까지 구현 범위를 제한했다.