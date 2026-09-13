# Embedded Communication Reliability Testbed

C++ 기반 Binary Communication Protocol을 설계하고 TCP / UDP 환경에서 통신 장애와 신뢰성 정책을 구현·검증하기 위한 프로젝트이다.

단순 Socket 통신 구현에 그치지 않고 다음 질문을 단계적으로 확인하는 것을 목표로 한다.

```text
TCP Byte Stream에서 메시지 경계는 어떻게 구분하는가?

Binary Protocol의 Field를 어떻게 직렬화하고 해석하는가?

Application ACK와 TCP 내부 ACK의 역할은 어떻게 다른가?

응답이 오지 않을 때 Timeout과 Retry를 어떻게 처리하는가?

연결이 끊어진 경우 Application은 어떻게 감지하고 복구하는가?

UDP에서 신뢰성이 필요하다면 Application이 무엇을 구현해야 하는가?

통신 장애를 어떻게 의도적으로 재현하고 검증할 수 있는가?
```

---

## 개발 단계

| Phase | 내용 | 상태 |
|---|---|---|
| Phase 1 | TCP Communication & Binary Protocol | ✅ |
| Phase 2 | Application Reliability | 예정 |
| Phase 3 | Fault Injection | 예정 |
| Phase 4 | UDP Reliability Extension | 예정 |
| Phase 5 | Test & Final Integration | 예정 |

### Phase 1 - TCP Communication & Binary Protocol

TCP Sender / Receiver를 구현하고 Binary Packet Format을 설계했다.

```text
[Magic][Version][Type][Sequence][Length][CRC32][Payload]
```

TCP의 Byte Stream 특성을 고려하여 Header의 Payload Length를 이용한 Framing을 구현했다.

또한 Serialization / Deserialization, Network Byte Order 변환, CRC32 검증과 Sequence 기반 DATA / ACK 통신을 구현했다.

상세 내용: `docs/phase-1.md`

---

## Phase 1 시스템 구조

```text
TCP Sender
    ↓
Packet
    ↓
encode_packet()
    ↓
Binary Protocol
    ↓
TCP Byte Stream
    ↓
TCP Receiver
    ↓
Header / Length 기반 Framing
    ↓
decode_packet()
    ↓
CRC Validation
    ↓
DATA 처리
    ↓
ACK
```

---

## Protocol Format

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

---

## Repository Structure

```text
embedded-comm-test/
├── README.md
├── CMakeLists.txt
├── docs/
│   └── phase-1.md
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

향후 Phase 진행에 따라 다음 디렉터리를 추가할 예정이다.

```text
udp/
fault_injector/
tests/
```

---

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

Receiver:

```bash
./build/tcp_receiver
```

Sender:

```bash
./build/tcp_sender
```

정상 실행 예시:

```text
Receiver:
Receiver listening on port 5000
Sender connected
Received DATA seq=1 payload=hello embedded
Sent ACK seq=1

Sender:
Connected to receiver
Sent DATA seq=1 payload=hello embedded
Received ACK seq=1
```

---

## Phase 1 검증

| Scenario | 결과 |
|---|---|
| Normal DATA → ACK | PASS |
| Payload CRC Corruption Detection | PASS |
| Normal Regression | PASS |

CRC 오류 검증에서는 Encoding 이후 Payload Byte를 의도적으로 변경하여 Receiver가 `CRC mismatch`로 Packet을 거부하는 것을 확인했다.

검증용 변조 코드는 테스트 이후 제거했다.

---

## Development Environment

```text
WSL2 Ubuntu
C++17
CMake
Linux Socket API
TCP/IP
```

현재 Phase 1은 동일 Host의 Loopback Interface(`127.0.0.1`)를 사용하여 Protocol 동작을 검증한다.
