# Embedded Communication Reliability Testbed

C++ 기반 Binary Communication Protocol을 직접 설계하고, TCP 통신에서 발생할 수 있는 장애를 재현하여 Application-level Reliability를 구현·검증하는 프로젝트이다.

단순한 Socket 송수신 구현이 아니라 다음 질문을 단계적으로 확인하는 것을 목표로 한다.

```text
TCP Byte Stream에서 Application Message 경계는 어떻게 구분하는가?

Binary Protocol은 어떻게 직렬화하고 검증하는가?

TCP 자체의 신뢰성과 Application ACK는 어떻게 다른가?

응답이 오지 않을 때 Timeout과 Retry를 어떻게 처리하는가?

Retry로 동일 메시지가 다시 전달될 때 중복 처리는 어떻게 방지하는가?

통신이 없는 동안 상대 Application이 정상적으로 응답 가능한지는 어떻게 확인하는가?

Connection이 끊어진 경우 어떻게 감지하고 통신을 복구하는가?

정상 Application 코드를 수정하지 않고 통신 장애를 어떻게 재현할 수 있는가?

UDP에서는 이러한 신뢰성 처리가 어떻게 달라지는가?
```

---

## 프로젝트 목표

이 프로젝트의 목적은 TCP 또는 UDP 자체를 다시 구현하는 것이 아니다.

Transport Layer 위에서 Application이 필요로 하는 다음 요소를 직접 구현하고 장애 상황에서 검증한다.

```text
Binary Protocol
Serialization / Deserialization
Message Framing
CRC Validation

Application ACK
Timeout
Retry
Duplicate Detection

Heartbeat
Connection Loss Detection
Reconnect

Fault Injection
Drop
Delay
Corruption
Disconnect
```

최종적으로 TCP와 UDP에서 Application Reliability가 어떻게 달라지는지 비교하는 것을 목표로 한다.

---

## 개발 단계

| Phase | 내용 | 상태 |
|---|---|---|
| Phase 1 | TCP Communication & Binary Protocol | ✅ 완료 |
| Phase 2 | Application Reliability | ✅ 완료 |
| Phase 3 | Fault Injection | ✅ 완료 |
| Phase 4 | UDP Reliability Extension | 예정 |
| Phase 5 | Test & Final Integration | 예정 |

---

# Phase 1 - TCP Communication & Binary Protocol

TCP Sender / Receiver를 구현하고 Application Binary Protocol을 설계했다.

초기에는 단순 문자열 통신으로 시작했다.

```text
Sender
  │
  │ "hello"
  ▼
Receiver
  │
  │ "ack"
  ▼
Sender
```

하지만 TCP는 Message 단위가 아니라 Byte Stream을 제공하므로 다음과 같은 가정을 할 수 없다.

```text
1 send()
=
1 recv()
```

따라서 고정 크기 Header에 Payload Length를 포함하는 Binary Protocol을 설계했다.

```text
[Header 16 byte][Payload N byte]
```

Receiver는 먼저 Header를 읽고 Length를 확인한 뒤 필요한 Payload만큼 추가로 수신한다.

```text
TCP Byte Stream
      ↓
Header 16 byte 수신
      ↓
Payload Length 확인
      ↓
Payload N byte 수신
      ↓
Packet Decode
```

Phase 1에서 구현한 주요 기능:

- TCP Client / Server
- Partial Send 처리
- Partial Receive 처리
- Length 기반 Message Framing
- Binary Serialization / Deserialization
- Network Byte Order
- Sequence Number
- CRC32 Validation

CRC 검증에서는 Encoding 이후 Payload를 의도적으로 변경하여 Receiver에서 `CRC mismatch`가 발생하는 것을 확인했다.

상세 내용:

```text
docs/phase-1.md
```

---

# Phase 2 - Application Reliability

Phase 1의 Binary Protocol 위에 Application-level Reliability를 추가했다.

TCP 자체에서도 ACK와 재전송을 사용하지만, 이는 Receiver Application이 실제 DATA를 처리했다는 의미와는 다르다.

따라서 별도의 Application ACK를 사용한다.

```text
Sender
  │
  │ DATA seq=1
  ▼
Receiver Application
  │
  │ DATA 처리
  │
  │ ACK seq=1
  ▼
Sender
```

## ACK Timeout / Retry

Sender는 DATA 전송 후 무한정 응답을 기다리지 않고 `poll()`을 사용해 제한 시간 동안 ACK를 기다린다.

```text
DATA
 ↓
ACK 대기
 │
 ├─ ACK 수신
 │    ↓
 │  정상 완료
 │
 └─ Timeout
      ↓
    Retry
```

현재 정책:

```text
ACK Timeout : 1000 ms
Max Retry   : 3회
```

최초 전송까지 포함하면 최대 4회 전송한다.

Retry 시에는 새로운 Sequence를 사용하지 않고 동일 Sequence를 유지한다.

```text
Attempt 1 → DATA seq=1
Attempt 2 → DATA seq=1
Attempt 3 → DATA seq=1
Attempt 4 → DATA seq=1
```

---

## Duplicate Detection

ACK가 Sender까지 전달되지 않았더라도 Receiver에서는 DATA 처리가 이미 완료되었을 수 있다.

```text
Sender                 Receiver

DATA seq=1 ──────────> 처리 완료
                         │
                         └─ ACK seq=1
                              X
                           응답 유실
```

Sender는 Timeout 후 같은 DATA를 다시 보낸다.

```text
DATA seq=1 Retry
```

Receiver는 처리한 Sequence를 기록하여 동일 Sequence의 DATA를 다시 처리하지 않는다.

```text
처음 seq=1
→ 실제 처리
→ Sequence 저장

다시 seq=1
→ Duplicate
→ 재처리하지 않음
→ ACK만 재전송
```

현재는 다음 자료구조를 사용한다.

```cpp
std::unordered_set<uint32_t> processed_sequences;
```

---

## Heartbeat

Application Traffic이 없는 상태에서도 상대가 실제로 응답 가능한 상태인지 확인하기 위해 Heartbeat를 추가했다.

```text
Sender
  │
  │ HEARTBEAT seq=N
  ▼
Receiver
  │
  │ HEARTBEAT_ACK seq=N
  ▼
Sender
```

`ACK`와 `HEARTBEAT_ACK`는 의미를 구분한다.

```text
ACK
= DATA가 Application에서 처리되었음을 확인

HEARTBEAT_ACK
= 상대 Application이 현재 요청에 응답 가능한 상태임을 확인
```

현재 정책:

```text
Heartbeat Interval : 2 sec
Heartbeat Timeout  : 1 sec
Heartbeat Count    : 3
```

---

## Reconnect

Heartbeat 응답이 제한 시간 안에 도착하지 않으면 기존 Connection을 더 이상 정상적인 통신 경로로 신뢰하지 않는다.

```text
HEARTBEAT
    ↓
Timeout
    ↓
기존 Socket 종료
    ↓
새 Socket 생성
    ↓
connect()
    ↓
동일 HEARTBEAT 재전송
```

Reconnect는 기존 TCP Connection을 되살리는 것이 아니라 새로운 TCP Connection을 생성하는 과정이다.

현재 정책:

```text
Reconnect Delay        : 2 sec
Max Reconnect Attempts : 3
```

상세 내용:

```text
docs/phase-2.md
```

---

# Phase 3 - Fault Injection

Phase 2에서는 ACK 누락이나 Heartbeat 응답 누락을 테스트하기 위해 Receiver 코드를 임시로 수정했다.

Phase 3에서는 정상 Sender / Receiver 코드를 유지한 채 통신 장애를 재현할 수 있도록 별도의 Fault Injector를 추가했다.

현재 시스템 구조:

```text
Sender
127.0.0.1:5000
      │
      ▼
┌─────────────────────┐
│    Fault Injector   │
│                     │
│ Server :5000        │
│      ↓              │
│ Protocol Decode     │
│      ↓              │
│ Fault Injection     │
│      ↓              │
│ Packet Relay        │
│      ↓              │
│ Client              │
└─────────┬───────────┘
          │
          │ 127.0.0.1:5001
          ▼
       Receiver
```

Fault Injector는 Sender에 대해서는 TCP Server, Receiver에 대해서는 TCP Client 역할을 한다.

따라서 실제로는 두 개의 TCP Connection이 존재한다.

```text
Connection #1

Sender ←────────────→ Fault Injector


Connection #2

Fault Injector ←────→ Receiver
```

---

## Protocol-aware Proxy

단순 Byte Relay가 아니라 Phase 1에서 만든 Binary Protocol을 Decode하여 Message Type과 Sequence를 확인한다.

```text
TCP Byte Stream
      ↓
Header / Payload 수신
      ↓
decode_packet()
      ↓
Packet Type / Sequence 확인
      ↓
Fault 적용 여부 판단
      ↓
Forward
```

이를 통해 다음과 같은 선택적인 장애 주입이 가능하다.

```text
ACK
→ Drop

ACK
→ Delay

DATA
→ Corruption

HEARTBEAT
→ Disconnect
```

Fault Injector는 `poll()`을 사용하여 Sender와 Receiver 방향의 Socket을 동시에 감시한다.

---

## Fault Modes

현재 하나의 실행파일에서 다음 Mode를 지원한다.

```text
none
drop-ack
delay-ack
corrupt-data
disconnect
```

실행 예:

```bash
./build/tcp_fault_injector none
./build/tcp_fault_injector drop-ack
./build/tcp_fault_injector delay-ack
./build/tcp_fault_injector corrupt-data
./build/tcp_fault_injector disconnect
```

---

## 3-1. Transparent Proxy

Fault를 적용하지 않고 모든 Packet을 정상적으로 양방향 Relay하는 것을 먼저 검증했다.

![Transparent TCP Proxy](docs/images/phase-3-transparent-proxy.png)

```text
DATA
→ ACK

HEARTBEAT
→ HEARTBEAT_ACK
```

Fault Injector가 추가된 이후에도 기존 TCP Protocol과 Application Reliability 기능이 정상 동작했다.

**Result: PASS**

---

## 3-2. ACK Message Drop

Receiver가 보낸 첫 번째 ACK를 Fault Injector가 수신한 뒤 Sender에게 전달하지 않았다.

![ACK Message Drop](docs/images/phase-3-ack-drop.png)

실제 흐름:

```text
Sender
  │ DATA seq=1
  ▼
Fault Injector
  │
  ▼
Receiver
  │
  │ DATA 처리
  │ ACK seq=1
  ▼
Fault Injector
  X ACK DROP

Sender
  │
  │ ACK Timeout
  │
  │ DATA seq=1 Retry
  ▼
Receiver
  │
  │ Duplicate DATA seq=1
  │ 실제 처리 X
  │ ACK seq=1
  ▼
Sender

ACK 수신
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
Sent ACK seq=1

Duplicate DATA seq=1 ignored
Sent ACK seq=1
```

이를 통해 다음 흐름을 검증했다.

```text
Application Message Drop
→ Timeout
→ Retry
→ Duplicate Detection
→ Recovery
```

**Result: PASS**

---

## 3-3. ACK Delay

첫 번째 ACK를 Fault Injector에서 700ms 지연시킨 뒤 Sender에게 전달했다.

![ACK Delay](docs/images/phase-3-delay.png)

현재 조건:

```text
Injected Delay = 700ms
ACK Timeout    = 1000ms
```

Fault Injector:

```text
[DELAY 700ms] ACK seq=1
[Receiver -> Sender] ACK seq=1
```

Sender는 Timeout 이전에 ACK를 수신했으므로 Retry하지 않았다.

```text
Sent DATA seq=1 attempt=1
Received ACK seq=1
```

Receiver에서도 Duplicate DATA가 발생하지 않았다.

```text
Delay < Timeout
→ 정상 ACK 처리
→ Retry 없음
```

**Result: PASS**

---

## 3-4. DATA Corruption

첫 DATA Packet을 정상적으로 Encoding한 뒤 Payload의 첫 Byte만 변경했다.

![DATA Corruption](docs/images/phase-3-corruption.png)

Fault Injector:

```text
[CORRUPT] DATA seq=1
```

Receiver:

```text
Receiver error: CRC mismatch
```

Sender:

```text
Sender error: peer disconnected
```

Corruption은 Packet 객체의 Payload를 먼저 수정하는 방식으로 구현하지 않았다.

그 경우 `encode_packet()`이 변경된 Payload 기준으로 CRC를 다시 계산하기 때문이다.

실제 구현은 다음 순서로 동작한다.

```text
정상 Packet
    ↓
encode_packet()
    ↓
정상 CRC 생성
    ↓
Payload 첫 Byte 변경
    ↓
CRC는 기존 값 유지
    ↓
Receiver
    ↓
CRC mismatch
```

손상된 DATA가 Application 처리 단계까지 전달되지 않는 것을 확인했다.

**Result: PASS**

---

## 3-5. Forced Disconnect

첫 Heartbeat가 Fault Injector에 도착했을 때 TCP Connection을 강제로 종료했다.

![Forced Disconnect](docs/images/phase-3-disconnect.png)

Fault Injector:

```text
[DISCONNECT] HEARTBEAT seq=2
Proxy connection closed: forced disconnect injected
```

Sender:

```text
Connection lost during HEARTBEAT seq=2 error=peer disconnected
Reconnecting...
Connected to receiver attempt=1
Resent HEARTBEAT seq=2
Received HEARTBEAT_ACK seq=2
```

Receiver:

```text
Sender disconnected
Waiting for connection

Sender connected
Received HEARTBEAT seq=2
Sent HEARTBEAT_ACK seq=2
```

기존 Phase 2에서는 Heartbeat Timeout만 Reconnect 대상으로 처리했다.

Forced Disconnect 테스트를 통해 실제 TCP Connection Loss도 별도로 처리해야 한다는 점을 확인했고 `ConnectionLost` 예외를 추가했다.

현재 Sender의 Recovery 조건은 다음과 같다.

```text
Heartbeat Timeout
        │
        ├──→ Reconnect
        │
Connection Lost
        │
        └──→ Reconnect
```

Reconnect 이후 실패했던 동일 `HEARTBEAT seq=2`를 다시 전송하고 정상 응답을 확인했다.

이후 `seq=3`, `seq=4` Heartbeat도 정상적으로 이어졌다.

**Result: PASS**

---

## Phase 3 정상 회귀 테스트

모든 Fault Mode 구현 이후 `none` Mode에서 전체 정상 동작을 다시 확인했다.

```bash
./build/tcp_fault_injector none
```

결과:

```text
DATA seq=1
→ ACK seq=1

HEARTBEAT seq=2
→ HEARTBEAT_ACK seq=2

HEARTBEAT seq=3
→ HEARTBEAT_ACK seq=3

HEARTBEAT seq=4
→ HEARTBEAT_ACK seq=4
```

Fault Injector 및 Connection Loss Recovery 기능 추가 이후에도 기존 정상 통신이 유지되는 것을 확인했다.

**Result: PASS**

상세 내용:

```text
docs/phase-3.md
```

---

# Protocol Format

Wire Format:

| Offset | Size | Field |
|---:|---:|---|
| 0 | 2 byte | Magic |
| 2 | 1 byte | Version |
| 3 | 1 byte | Type |
| 4 | 4 byte | Sequence |
| 8 | 4 byte | Payload Length |
| 12 | 4 byte | CRC32 |
| 16 | N byte | Payload |

Header Size:

```text
16 bytes
```

Protocol 기본값:

```text
Magic       : 0xAA55
Version     : 1
Max Payload : 1024 bytes
```

Message Type:

```text
DATA          = 1
ACK           = 2
HEARTBEAT     = 3
HEARTBEAT_ACK = 4
```

CRC32는 다음 Polynomial 기반으로 구현했다.

```text
0xEDB88320
```

CRC 계산 대상:

```text
Magic
Version
Type
Sequence
Payload Length
Payload
```

CRC Field 자체는 CRC 계산 대상에서 제외한다.

---

# Current Architecture

현재 Phase 3 기준 실행 구조는 다음과 같다.

```text
                  Application Protocol
                         │
                         ▼
┌────────────┐     ┌──────────────────┐     ┌────────────┐
│   Sender   │     │  Fault Injector  │     │  Receiver  │
│            │     │                  │     │            │
│ DATA       │────>│ Decode           │────>│ DATA       │
│ HEARTBEAT  │     │ Fault Injection  │     │            │
│            │<────│ Packet Relay     │<────│ ACK        │
│            │     │                  │     │ HEARTBEAT  │
│            │     │                  │     │ ACK        │
└────────────┘     └──────────────────┘     └────────────┘
      │                    │                      │
    :5000                :5000                  :5001
```

Application Reliability:

```text
DATA
  ↓
ACK
  │
  └─ 응답 없음
       ↓
     Timeout
       ↓
     Retry
       ↓
Duplicate Detection
```

Liveness / Recovery:

```text
HEARTBEAT
    ↓
HEARTBEAT_ACK
    │
    ├─ Timeout
    │    ↓
    │ Reconnect
    │
    └─ Connection Lost
         ↓
       Reconnect
```

---

# Repository Structure

```text
embedded-comm-test/
├── CMakeLists.txt
├── README.md
│
├── docs/
│   ├── images/
│   │   ├── phase-3-transparent-proxy.png
│   │   ├── phase-3-ack-drop.png
│   │   ├── phase-3-delay.png
│   │   ├── phase-3-corruption.png
│   │   └── phase-3-disconnect.png
│   │
│   ├── phase-1.md
│   ├── phase-2.md
│   └── phase-3.md
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
└── fault_injector/
    └── tcp_proxy.cpp
```

향후 Phase에서 다음 디렉터리를 추가할 예정이다.

```text
udp/
tests/
```

---

# Build

```bash
cmake -S . -B build
cmake --build build
```

생성되는 실행파일:

```text
build/tcp_sender
build/tcp_receiver
build/tcp_fault_injector
```

---

# Run

현재 구조에서는 Receiver → Fault Injector → Sender 순서로 실행한다.

## 1. Receiver

```bash
./build/tcp_receiver
```

Receiver는 실제 Application Server로 `5001` Port를 사용한다.

```text
Receiver listening on port 5001
```

---

## 2. Fault Injector

정상 Relay:

```bash
./build/tcp_fault_injector none
```

또는 원하는 Fault Mode를 선택한다.

```bash
./build/tcp_fault_injector drop-ack
./build/tcp_fault_injector delay-ack
./build/tcp_fault_injector corrupt-data
./build/tcp_fault_injector disconnect
```

Fault Injector는 Sender를 위해 `5000` Port에서 대기한다.

---

## 3. Sender

```bash
./build/tcp_sender
```

Sender는 기존과 동일하게 `127.0.0.1:5000`으로 연결한다.

Sender는 Fault Injector가 중간에 존재하는지 알 필요가 없다.

---

# Test Results

| Scenario | 검증 내용 | 결과 |
|---|---|---|
| TCP Framing | Length 기반 Packet 경계 처리 | PASS |
| CRC Validation | 손상 Payload 검출 | PASS |
| Application ACK | DATA 처리 완료 확인 | PASS |
| ACK Timeout | 1초 응답 제한 | PASS |
| Retry | 동일 Sequence 재전송 | PASS |
| Duplicate Detection | 동일 DATA 재처리 방지 | PASS |
| Heartbeat | Application Liveness 확인 | PASS |
| Heartbeat Timeout | 응답 불능 상태 감지 | PASS |
| Transparent Proxy | Proxy 경유 정상 통신 | PASS |
| ACK Drop | Message Drop 재현 | PASS |
| ACK Delay | 700ms 지연 재현 | PASS |
| DATA Corruption | 중간 Payload 변조 및 CRC 검출 | PASS |
| Forced Disconnect | 실제 TCP Connection 강제 종료 | PASS |
| Connection Loss Detection | Sender의 연결 종료 감지 | PASS |
| Reconnect | 새로운 TCP Connection 생성 | PASS |
| Recovery | Reconnect 이후 통신 지속 | PASS |
| Normal Regression | Fault 비활성 상태 정상 동작 | PASS |

---

# 구현을 통해 확인한 내용

현재까지 직접 구현하고 검증한 요소는 다음과 같다.

```text
Linux Socket API

socket
bind
listen
accept
connect
send
recv
poll
shutdown

TCP Client / Server
TCP Byte Stream
Partial Send / Receive

Binary Protocol
Serialization / Deserialization
Network Byte Order
Length-based Framing
CRC32

Sequence Number
Application ACK
Timeout
Retry
Duplicate Detection

Heartbeat
Application-level Liveness
Connection Loss Detection
Reconnect

TCP Proxy
Protocol-aware Relay

Fault Injection
ACK Drop
ACK Delay
DATA Corruption
Forced Disconnect
```

---

# 설계 과정에서 확인한 점

## TCP Reliability와 Application Reliability는 다르다

TCP가 신뢰성 있는 Byte Stream을 제공하더라도 다음 질문까지 해결해주지는 않는다.

```text
Receiver Application이 실제 DATA를 처리했는가?

응답이 없는 경우 얼마나 기다릴 것인가?

Retry된 동일 메시지를 다시 처리할 것인가?

상대 Application이 실제로 응답 가능한 상태인가?

Connection이 끊어진 경우 Application은 어떻게 복구할 것인가?
```

따라서 Application 수준에서 별도의 정책이 필요했다.

---

## Retry는 Duplicate 가능성을 만든다

```text
Timeout
→ Retry
```

만 구현하면 동일 작업이 여러 번 수행될 수 있다.

따라서:

```text
Retry
→ Same Sequence
→ Duplicate Detection
```

을 함께 설계했다.

---

## Connection 존재와 Application 정상 상태는 다르다

TCP Connection이 존재하더라도 Application이 정상적으로 응답한다는 보장은 없다.

따라서 별도의:

```text
HEARTBEAT
→ HEARTBEAT_ACK
```

를 사용하여 Application Liveness를 확인했다.

---

## Timeout과 Connection Loss도 다르다

Phase 3의 Forced Disconnect를 통해 다음 두 상황을 구분할 필요가 있음을 확인했다.

```text
응답은 없지만 Connection은 존재
→ Timeout

TCP Connection 자체가 종료
→ Connection Lost
```

두 상황 모두 현재 정책에서는 Reconnect로 이어지지만 감지 방식은 다르게 구현했다.

---

## Corruption은 Wire-format 단계에서 발생시켜야 했다

Packet 객체의 Payload를 변경한 뒤 다시 Encoding하면 CRC도 함께 갱신된다.

따라서 실제 데이터 손상 검증을 위해:

```text
Encoding
→ CRC 생성
→ Binary Payload 변경
→ Receiver CRC mismatch
```

순서로 장애를 주입했다.

---

# 현재 구현의 범위와 한계

현재 `processed_sequences`는 Receiver Process Memory에 저장한다.

따라서 Receiver Process 자체가 재시작되면 Deduplication 정보가 사라진다.

또한 새로운 논리 Session에서 Sequence가 다시 `1`부터 시작할 경우 이전 Session의 Sequence와 충돌할 수 있다.

이를 확장하려면 다음과 같은 요소가 필요할 수 있다.

```text
Session ID
Persistent Deduplication State
Sequence Scope 관리
```

현재 Fault Injector 역시 범용 Network Emulator가 아니라 프로젝트 검증을 위한 단일 Process / 단일 Connection 중심의 Test Proxy이다.

특히 Delay Mode는 `sleep_for()` 동안 Relay Thread가 Block된다.

이번 프로젝트에서는 Traffic Control Framework 자체를 구현하기보다, 통신 장애를 통제된 조건에서 재현하고 Reliability 정책을 검증하는 데 범위를 제한했다.

---

# Next Phase

## Phase 4 - UDP Reliability Extension

동일 Binary Protocol을 UDP로 확장한다.

TCP와 달리 UDP에서는 Transport Layer가 다음 기능을 제공하지 않는다.

```text
Connection
Reliable Delivery
Automatic Retransmission
Ordering
Duplicate Prevention
```

따라서 UDP에서는 Application에서 직접 다음 기능을 구현한다.

```text
Datagram Send / Receive
Application ACK
Timeout
Retry
Duplicate Detection
Out-of-order Detection
```

이후 TCP 구현과 비교하여 Transport 특성에 따라 Application Reliability 설계가 어떻게 달라지는지 정리한다.

---

## Phase 5 - Test & Final Integration

마지막 Phase에서는 전체 시나리오를 통합 검증하고 다음 내용을 정리한다.

```text
TCP / UDP Reliability 비교

정상 통신
Message Drop
Delay
Corruption
Disconnect / Loss

전체 Test Matrix

최종 Architecture
최종 README / 문서
```