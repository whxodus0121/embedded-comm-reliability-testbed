# Embedded Communication Reliability Testbed

C++ 기반으로 Binary Protocol을 설계하고 TCP / UDP 통신에서 발생할 수 있는 Timeout, Retry, Duplicate, Corruption, Disconnect, Out-of-order 상황을 직접 재현하고 검증하는 프로젝트입니다.

단순한 Socket 통신 구현이 아니라 다음 질문을 직접 구현과 테스트를 통해 확인하는 것을 목표로 합니다.

- TCP Byte Stream에서 Application Message 경계를 어떻게 구분할 것인가?
- Application이 메시지 처리 여부를 어떻게 확인할 것인가?
- ACK가 손실되면 어떻게 복구할 것인가?
- 재전송으로 발생하는 중복 처리를 어떻게 방지할 것인가?
- 통신 중 연결이 끊기면 어떻게 복구할 것인가?
- 데이터가 손상되었을 때 어떻게 감지할 것인가?
- UDP에서는 TCP와 달리 어떤 신뢰성 기능을 Application이 직접 구현해야 하는가?
- UDP Datagram이 순서대로 도착하지 않을 경우 어떻게 감지할 것인가?

---

## Development Progress

| Phase | 내용 | 상태 |
|---|---|---|
| Phase 1 | TCP Communication & Binary Protocol | ✅ Complete |
| Phase 2 | Application Reliability | ✅ Complete |
| Phase 3 | Fault Injection | ✅ Complete |
| Phase 4 | UDP Reliability Extension | ✅ Complete |
| Phase 5 | Test & Final Integration | Planned |

---

# Phase 1 - TCP Communication & Binary Protocol

Phase 1에서는 TCP 통신 위에서 사용할 Application Binary Protocol을 직접 설계했습니다.

TCP는 Message 단위가 아니라 Byte Stream을 제공하기 때문에 한 번의 `send()`와 한 번의 `recv()`가 동일한 Message 경계를 보장하지 않습니다.

따라서 고정 크기 Header에 Payload Length를 포함하고, Receiver가 Header를 먼저 읽은 뒤 Payload Length만큼 추가로 읽는 구조를 사용했습니다.

Binary Protocol 형식은 다음과 같습니다.

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

Protocol Header 크기는 16 Byte이며 최대 Payload 크기는 1024 Byte입니다.

```text
Magic        = 0xAA55
Version      = 1
Header Size  = 16 bytes
Max Payload  = 1024 bytes
```

지원 Message Type:

```text
Data         = 1
Ack          = 2
Heartbeat    = 3
HeartbeatAck = 4
```

Host 환경에 따라 Byte Order가 달라지는 문제를 방지하기 위해 Multi-byte Integer는 Network Byte Order로 변환합니다.

```text
htons / htonl
ntohs / ntohl
```

C/C++ 구조체 자체를 Socket으로 직접 전송하지 않고 명시적으로 Serialize / Deserialize하도록 구현하여 Padding, Alignment, Endianness 문제를 피했습니다.

또한 CRC32를 사용해 Packet 손상을 검증합니다.

CRC 계산에는 다음 Field가 포함됩니다.

```text
Magic
Version
Type
Sequence
Payload Length
Payload
```

CRC Field 자체는 CRC 계산에서 제외됩니다.

TCP Receiver에서는 먼저 16 Byte Header를 정확히 읽고 Payload Length를 확인한 뒤 해당 크기만큼 Payload를 추가로 수신합니다.

```text
TCP Byte Stream
        ↓
16 Byte Header
        ↓
Payload Length 확인
        ↓
Payload Length만큼 추가 수신
        ↓
Packet Decode / CRC 검증
```

Phase 1을 통해 Binary Protocol Encoding / Decoding, TCP Framing, Network Byte Order, CRC32 검증을 구현했습니다.

상세 내용은 [`docs/phase-1.md`](docs/phase-1.md)에 정리했습니다.

---

# Phase 2 - Application Reliability

Phase 2에서는 TCP Transport가 제공하는 신뢰성과 별도로 Application Message 처리 여부를 확인할 수 있도록 ACK / Timeout / Retry 구조를 구현했습니다.

TCP 자체에도 ACK와 재전송 기능이 존재하지만 이는 TCP Byte Stream 전달을 위한 Transport Layer 기능입니다.

이 프로젝트에서 사용하는 `MessageType::Ack`는 Receiver Application이 특정 Sequence의 DATA를 수신하고 처리했다는 것을 확인하기 위한 별도의 Application Layer ACK입니다.

```text
Sender                              Receiver

DATA seq=1  ----------------------->

                     Application 처리

            <----------------------- ACK seq=1
```

Sender는 ACK를 일정 시간 기다립니다.

```text
ACK Timeout = 1000 ms
Max Retry   = 3
```

초기 전송 1회와 추가 Retry 최대 3회를 허용하므로 최대 전송 횟수는 4회입니다.

Retry가 발생해도 동일한 논리 Message에는 동일한 Sequence를 사용합니다.

```text
DATA seq=1 attempt=1
        ↓
Timeout
        ↓
DATA seq=1 attempt=2
```

Receiver는 이미 처리한 Sequence를 기록하고 동일한 Sequence가 다시 전달되면 실제 처리를 반복하지 않습니다.

```text
First DATA seq=1
→ 처리
→ Sequence 기록

Retry DATA seq=1
→ Duplicate 감지
→ 처리 생략
→ ACK 재전송
```

또한 연결 상태 확인을 위해 Heartbeat / HeartbeatAck를 추가했습니다.

```text
HEARTBEAT seq=N
       ↓
HEARTBEAT_ACK seq=N
```

Heartbeat가 실패하거나 TCP Connection이 끊어진 경우 기존 Socket을 닫고 새로운 TCP Connection을 생성하여 다시 연결하도록 구현했습니다.

```text
Connection Lost
      ↓
Old Socket Close
      ↓
New TCP Connection
      ↓
Same Heartbeat Retry
      ↓
Recovery
```

상세 내용은 [`docs/phase-2.md`](docs/phase-2.md)에 정리했습니다.

---

# Phase 3 - Fault Injection

Phase 3에서는 정상적인 localhost 통신만으로는 재현하기 어려운 장애 상황을 통제된 환경에서 반복적으로 검증하기 위해 Protocol-aware TCP Fault Injector를 구현했습니다.

전체 구조는 다음과 같습니다.

```text
TCP Sender                Fault Injector                TCP Receiver

connect :5000  ------->   listen :5000
                               |
                               | TCP connection
                               v
                          Receiver :5001
```

하나의 TCP Connection 중간에 Injector를 삽입한 것이 아니라 실제로는 두 개의 TCP Connection으로 구성됩니다.

```text
Sender ↔ Fault Injector
Fault Injector ↔ Receiver
```

Fault Injector는 Application Binary Protocol을 Decode하여 Message Type과 Sequence를 확인하고 선택적으로 장애를 주입합니다.

지원 Fault Mode:

```text
none
drop-ack
delay-ack
corrupt-data
disconnect
```

실행 예시:

```bash
./build/tcp_fault_injector none
./build/tcp_fault_injector drop-ack
./build/tcp_fault_injector delay-ack
./build/tcp_fault_injector corrupt-data
./build/tcp_fault_injector disconnect
```

---

## Transparent Proxy

Fault를 적용하지 않은 경우 Sender와 Receiver 사이의 DATA / ACK / HEARTBEAT을 그대로 Relay합니다.

![Transparent Proxy](docs/images/phase-3-transparent-proxy.png)

---

## ACK Drop

Receiver는 ACK를 정상적으로 생성하지만 Fault Injector가 첫 ACK를 전달하지 않습니다.

```text
Sender                  Injector                  Receiver

DATA seq=1  ---------->           -------------->
                                              DATA 처리

                                            ACK seq=1
                          <------------

                     X ACK Drop

ACK Timeout

DATA seq=1  ---------->           -------------->

                                      Duplicate 감지

                                            ACK seq=1
                         <-------------  <--------

Received ACK
```

Sender:

```text
Sent DATA seq=1 attempt=1
ACK timeout seq=1
Sent DATA seq=1 attempt=2
Received ACK seq=1
```

Receiver:

```text
Received DATA seq=1
Duplicate DATA seq=1 ignored
```

![ACK Drop](docs/images/phase-3-ack-drop.png)

---

## ACK Delay

첫 ACK를 700ms 지연시켰습니다.

ACK Timeout은 1000ms이므로 Retry가 발생하지 않는 범위에서 순수한 Latency 상황을 검증했습니다.

```text
[DELAY 700ms] ACK seq=1
```

![ACK Delay](docs/images/phase-3-delay.png)

---

## DATA Corruption

DATA Packet을 Encode한 이후 첫 Payload Byte를 변경하여 기존 CRC와 실제 Payload가 일치하지 않도록 만들었습니다.

```text
Encode Packet
     ↓
CRC 계산 완료
     ↓
Payload Byte 변경
     ↓
Receiver
     ↓
CRC mismatch
```

Packet을 변경한 뒤 다시 Encode하면 CRC까지 새로 계산되므로 Corruption을 감지할 수 없습니다. 따라서 Encoding 이후 실제 전송 Byte를 변경했습니다.

Receiver:

```text
Receiver error: CRC mismatch
```

![DATA Corruption](docs/images/phase-3-corruption.png)

---

## Forced Disconnect

첫 Heartbeat를 전달하기 전에 Fault Injector가 두 TCP Connection을 강제로 종료하도록 구현했습니다.

Sender는 연결 종료를 감지하고 새로운 TCP Connection을 생성한 뒤 동일 Heartbeat Sequence를 다시 전송합니다.

```text
HEARTBEAT seq=2
       ↓
Forced Disconnect
       ↓
Connection Lost
       ↓
Reconnect
       ↓
HEARTBEAT seq=2 Retry
       ↓
HEARTBEAT_ACK seq=2
```

Sender:

```text
Connection lost during HEARTBEAT seq=2
Reconnecting...
Connected to receiver attempt=1
Resent HEARTBEAT seq=2
Received HEARTBEAT_ACK seq=2
```

![Forced Disconnect](docs/images/phase-3-disconnect.png)

상세 내용은 [`docs/phase-3.md`](docs/phase-3.md)에 정리했습니다.

---

# Phase 4 - UDP Reliability Extension

Phase 4에서는 기존 Binary Protocol을 UDP 환경으로 확장했습니다.

Protocol 자체는 변경하지 않고 기존 `encode_packet()` / `decode_packet()` 및 CRC32 검증을 그대로 재사용했습니다.

TCP와 UDP의 가장 큰 구현 차이 중 하나는 데이터 경계입니다.

TCP는 Byte Stream이므로 Application에서 Message Framing이 필요합니다.

```text
TCP

send(Packet A)
send(Packet B)

        ↓

AAAAAAAABBBBBBBB...
```

UDP는 Datagram 경계를 유지합니다.

```text
UDP

sendto(Packet A)
sendto(Packet B)

        ↓

[ Datagram A ]
[ Datagram B ]
```

따라서 UDP에서는 TCP에서 사용했던 `recv_exact()` 기반 Header / Payload Framing이 필요하지 않습니다.

Receiver는 `recvfrom()`으로 하나의 Datagram을 받은 뒤 실제 Datagram 크기와 Protocol Header의 Payload Length가 일치하는지 검증합니다.

반면 UDP는 다음 기능을 제공하지 않습니다.

- 전달 보장
- 자동 재전송
- 순서 보장
- 중복 제거

따라서 필요한 신뢰성 기능을 Application에서 직접 구현했습니다.

---

## UDP Basic Communication

UDP Sender와 Receiver는 직접 연결되며 Receiver는 Port 6000을 사용합니다.

```text
UDP Sender
    │
    │ DATA Datagram
    ▼
UDP Receiver :6000
    │
    │ ACK Datagram
    ▼
UDP Sender
```

기존 Binary Protocol을 그대로 사용하여 DATA와 ACK를 정상적으로 송수신했습니다.

![UDP Basic Communication](docs/images/phase-4-udp-basic.png)

---

## ACK Timeout / Retry / Duplicate Detection

Sender는 DATA를 전송한 뒤 `poll()`을 이용해 최대 1000ms 동안 ACK를 기다립니다.

```text
DATA seq=1
    ↓
ACK 대기
    ↓
Timeout
    ↓
DATA seq=1 Retry
```

최대 Retry 횟수는 3회이며 Retry에서도 동일한 Sequence를 유지합니다.

ACK Loss 상황을 재현하기 위해 UDP Receiver에 `drop-first-ack` Test Mode를 추가했습니다.

```bash
./build/udp_receiver drop-first-ack
```

Receiver는 첫 DATA를 정상 처리하지만 첫 ACK는 의도적으로 전송하지 않습니다.

Sender:

```text
Sent DATA seq=1 attempt=1 payload=hello udp
ACK timeout seq=1
Sent DATA seq=1 attempt=2 payload=hello udp
Received ACK seq=1
```

Receiver:

```text
Received DATA seq=1 payload=hello udp
ACK intentionally dropped seq=1
Duplicate DATA seq=1 ignored
Sent ACK seq=1
```

Receiver는 이미 처리한 Sequence를 기록하고 동일 Sequence가 다시 들어오면 실제 처리는 반복하지 않습니다.

```text
ACK Loss
    ↓
Timeout
    ↓
Retry
    ↓
Duplicate Detection
    ↓
ACK 재전송
    ↓
Recovery
```

![UDP Retry and Duplicate Detection](docs/images/phase-4-udp-retry.png)

---

## Out-of-order Detection

UDP는 Datagram의 전달 순서를 보장하지 않습니다.

localhost 환경에서 실제 Packet Reordering 발생을 기다리는 방식은 재현성이 낮기 때문에 Sender에서 의도적으로 다음 순서로 DATA를 전송했습니다.

```text
1 → 3 → 2
```

실행:

```bash
./build/udp_receiver out-of-order
./build/udp_sender out-of-order
```

Receiver는 다음에 받아야 하는 Sequence를 추적합니다.

```text
expected_sequence = 1
```

`seq=1` 수신 후:

```text
expected_sequence = 2
```

인 상태에서 `seq=3`이 먼저 들어오면:

```text
Out-of-order DATA seq=3 expected=2
```

를 출력합니다.

실제 결과:

```text
Received DATA seq=1 payload=message-1
Sent ACK seq=1

Out-of-order DATA seq=3 expected=2
Received DATA seq=3 payload=message-3
Sent ACK seq=3

Received DATA seq=2 payload=message-2
Sent ACK seq=2
```

![UDP Out-of-order Detection](docs/images/phase-4-udp-out-of-order.png)

이 테스트는 네트워크가 실제로 Datagram을 재정렬했다는 의미가 아니라 UDP 환경에서 발생할 수 있는 순서 역전 상황을 통제된 입력으로 재현하여 Detection Logic을 검증한 것입니다.

이번 구현에서는 Out-of-order Detection까지만 수행하며 별도의 Reordering Buffer나 Sliding Window는 구현하지 않았습니다.

상세 내용은 [`docs/phase-4.md`](docs/phase-4.md)에 정리했습니다.

---

# Protocol

공통 Binary Protocol:

```text
┌──────────────┬─────────┬──────────────────────────────┐
│ Offset       │ Size    │ Field                        │
├──────────────┼─────────┼──────────────────────────────┤
│ 0            │ 2       │ Magic                        │
│ 2            │ 1       │ Version                      │
│ 3            │ 1       │ Message Type                 │
│ 4            │ 4       │ Sequence                     │
│ 8            │ 4       │ Payload Length               │
│ 12           │ 4       │ CRC32                        │
│ 16           │ N       │ Payload                      │
└──────────────┴─────────┴──────────────────────────────┘
```

TCP와 UDP 모두 동일한 Protocol Codec을 사용합니다.

```text
Application Packet
       │
       ▼
encode_packet()
       │
       ▼
Binary Protocol
       │
       ├──────── TCP
       │
       └──────── UDP
```

Transport가 달라져도 Application Protocol Format과 CRC 검증 로직은 공유합니다.

---

# Current Architecture

```text
                         Binary Protocol
                              │
                 ┌────────────┴────────────┐
                 │                         │
                 ▼                         ▼

              TCP Path                  UDP Path

        ┌─────────────────┐        ┌─────────────────┐
        │   TCP Sender    │        │   UDP Sender    │
        └────────┬────────┘        └────────┬────────┘
                 │ :5000                    │
                 ▼                          │ Datagram
        ┌─────────────────┐                 ▼
        │ Fault Injector  │        ┌─────────────────┐
        │     :5000       │        │  UDP Receiver   │
        └────────┬────────┘        │     :6000       │
                 │                 └─────────────────┘
                 │ :5001
                 ▼
        ┌─────────────────┐
        │  TCP Receiver   │
        │     :5001       │
        └─────────────────┘
```

---

# Project Structure

```text
embedded-comm-test/
├── CMakeLists.txt
├── README.md
│
├── protocol/
│   ├── packet.hpp
│   ├── codec.hpp
│   ├── codec.cpp
│   ├── crc32.hpp
│   └── crc32.cpp
│
├── tcp/
│   ├── sender.cpp
│   └── receiver.cpp
│
├── udp/
│   ├── sender.cpp
│   └── receiver.cpp
│
├── fault_injector/
│   └── tcp_proxy.cpp
│
└── docs/
    ├── phase-1.md
    ├── phase-2.md
    ├── phase-3.md
    ├── phase-4.md
    └── images/
        ├── phase-3-transparent-proxy.png
        ├── phase-3-ack-drop.png
        ├── phase-3-delay.png
        ├── phase-3-corruption.png
        ├── phase-3-disconnect.png
        ├── phase-4-udp-basic.png
        ├── phase-4-udp-retry.png
        └── phase-4-udp-out-of-order.png
```

---

# Build

```bash
cmake -S . -B build
cmake --build build
```

생성되는 주요 실행 파일:

```text
build/tcp_sender
build/tcp_receiver
build/tcp_fault_injector

build/udp_sender
build/udp_receiver
```

---

# Run

## TCP Normal

Receiver:

```bash
./build/tcp_receiver
```

Fault Injector:

```bash
./build/tcp_fault_injector none
```

Sender:

```bash
./build/tcp_sender
```

실행 순서는 다음과 같습니다.

```text
TCP Receiver
→ Fault Injector
→ TCP Sender
```

---

## TCP Fault Injection

ACK Drop:

```bash
./build/tcp_fault_injector drop-ack
```

ACK Delay:

```bash
./build/tcp_fault_injector delay-ack
```

DATA Corruption:

```bash
./build/tcp_fault_injector corrupt-data
```

Forced Disconnect:

```bash
./build/tcp_fault_injector disconnect
```

각 테스트를 다시 수행하려면 Fault Injector를 재시작합니다.

---

## UDP Normal

Receiver:

```bash
./build/udp_receiver
```

Sender:

```bash
./build/udp_sender
```

---

## UDP ACK Loss / Retry

Receiver:

```bash
./build/udp_receiver drop-first-ack
```

Sender:

```bash
./build/udp_sender
```

---

## UDP Out-of-order

Receiver:

```bash
./build/udp_receiver out-of-order
```

Sender:

```bash
./build/udp_sender out-of-order
```

---

# Test Results

| Transport | Test | 검증 내용 | 결과 |
|---|---|---|---|
| TCP | Normal | Binary Protocol DATA / ACK | PASS |
| TCP | CRC | 손상된 Payload 감지 | PASS |
| TCP | ACK Timeout | Application ACK 미수신 감지 | PASS |
| TCP | Retry | 동일 Sequence 재전송 | PASS |
| TCP | Duplicate | 재전송 Message 중복 처리 방지 | PASS |
| TCP | Heartbeat | 연결 상태 확인 | PASS |
| TCP | Reconnect | TCP 연결 종료 후 재연결 | PASS |
| TCP | ACK Drop | Fault Injector에서 ACK 폐기 | PASS |
| TCP | ACK Delay | Application ACK 지연 | PASS |
| TCP | Corruption | Encode 이후 Payload 손상 / CRC 감지 | PASS |
| TCP | Disconnect | 강제 연결 종료 후 Recovery | PASS |
| UDP | Basic | Datagram 기반 DATA / ACK | PASS |
| UDP | ACK Timeout | ACK 손실 후 Timeout | PASS |
| UDP | Retry | 동일 Sequence Datagram 재전송 | PASS |
| UDP | Duplicate | Retry로 발생한 중복 처리 방지 | PASS |
| UDP | Recovery | Retry 이후 ACK 수신 | PASS |
| UDP | Out-of-order | 예상 Sequence보다 큰 Datagram 선도착 감지 | PASS |

---

# TCP vs UDP

| 항목 | TCP | UDP |
|---|---|---|
| Socket Type | `SOCK_STREAM` | `SOCK_DGRAM` |
| 통신 형태 | Connection-oriented | Connectionless |
| 데이터 형태 | Byte Stream | Datagram |
| Message 경계 | 보장하지 않음 | 유지 |
| `listen()` / `accept()` | 필요 | 필요 없음 |
| 기본 송수신 | `send()` / `recv()` | `sendto()` / `recvfrom()` |
| Transport 전달 보장 | 제공 | 제공하지 않음 |
| Transport 재전송 | 제공 | 제공하지 않음 |
| 순서 보장 | 제공 | 제공하지 않음 |
| Application ACK | 처리 확인을 위해 구현 | 전달 / 처리 확인을 위해 구현 |
| Application Retry | Application 처리 실패 대응 | Datagram 손실 복구까지 담당 |
| Duplicate Detection | App Retry 때문에 필요 | App Retry 때문에 필요 |
| Framing | 직접 필요 | Datagram 경계 활용 |
| Reconnect | 필요 | TCP와 같은 Connection 개념 없음 |

TCP에서도 Application ACK를 구현한 이유는 TCP ACK가 Receiver Application의 실제 Message 처리 성공을 의미하지 않기 때문입니다.

UDP에서는 Transport Layer 자체의 재전송 기능도 없으므로 Application ACK / Timeout / Retry가 데이터 손실 복구에 더 직접적으로 사용됩니다.

---

# Key Design Points

### Binary Protocol을 직접 설계

단순 문자열 송수신 대신 Header / Sequence / Length / CRC를 포함한 Binary Protocol을 구현했습니다.

### Transport와 Protocol 분리

TCP와 UDP 모두 동일한 `Packet`, `encode_packet()`, `decode_packet()`을 사용하도록 구성했습니다.

### Sequence 기반 Reliability

Sequence를 다음 목적으로 사용합니다.

```text
ACK Matching
Retry Identification
Duplicate Detection
Out-of-order Detection
```

### Application ACK와 TCP ACK 구분

TCP 자체의 ACK와 Application Message 처리 확인용 ACK를 분리했습니다.

### Controlled Fault Injection

무작위 네트워크 장애에 의존하지 않고 Fault Injector와 Test Mode를 사용하여 장애 상황을 반복 가능하게 재현했습니다.

### Corruption은 Encoding 이후 적용

Packet을 수정한 뒤 다시 Encode하면 CRC도 새 Payload 기준으로 변경되기 때문에 실제 전송 Byte를 Encoding 이후 변경하여 CRC Error를 재현했습니다.

### 최소 범위 유지

통신 신뢰성의 핵심 동작을 직접 확인하는 것이 목적이므로 범위를 다음 수준으로 제한했습니다.

구현 범위:

```text
Binary Protocol
CRC32
ACK
Timeout
Retry
Duplicate Detection
Heartbeat
Reconnect
Fault Injection
UDP Reliability
Out-of-order Detection
```

제외 범위:

```text
epoll
Multi-client Server
Thread Pool
TLS
MQTT
Protocol Buffers
Sliding Window
Selective ACK
Congestion Control
Out-of-order Reordering Buffer
RTT 기반 Dynamic Timeout
Persistent Session ID
Reliable UDP Protocol 전체 구현
```

---

# Current Limitations

현재 구현은 통신 신뢰성 메커니즘을 학습하고 검증하기 위한 Testbed입니다.

TCP Receiver의 Duplicate State는 Process Memory에만 유지됩니다. 따라서 Receiver가 재시작되거나 완전히 새로운 논리 Session에서 Sequence가 다시 1부터 시작하는 경우를 구분하기 위한 Session ID 또는 Persistent Sequence는 구현하지 않았습니다.

TCP Fault Injector는 테스트 목적의 단일 Thread Proxy입니다. ACK Delay 과정에서 Relay Thread가 함께 Block되므로 범용 Network Emulator가 아니라 통제된 장애 재현 도구로 사용합니다.

UDP Receiver 역시 단일 Sender 테스트를 기준으로 구현했습니다. 여러 Sender가 동일 Sequence 공간을 사용할 경우 Sender별 Session State를 분리해야 합니다.

UDP Out-of-order 테스트는 실제 Network Reordering을 측정한 것이 아니라 `1 → 3 → 2` 순서의 통제된 입력을 통해 Detection Logic을 검증한 것입니다.

---

# Next Phase

## Phase 5 - Test & Final Integration

Phase 5에서는 새로운 통신 기능을 추가하기보다 지금까지 구현한 기능을 전체적으로 검증하고 프로젝트를 마무리합니다.

예정 작업:

- TCP 전체 Regression Test
- UDP 전체 Regression Test
- Fault Injection Scenario 최종 검증
- Test Matrix 정리
- TCP / UDP Reliability 비교 정리
- 최종 Architecture 정리
- README 및 문서 최종 점검