# Phase 3 - Fault Injection

## 1. 목표

Phase 2에서는 ACK 누락과 Heartbeat 응답 누락을 Receiver 코드 내부에서 직접 발생시켜 Timeout, Retry, Duplicate Detection, Reconnect를 검증했다.

하지만 이 방식은 장애를 재현할 때마다 정상 Application 코드를 수정해야 한다는 문제가 있다.

Phase 3에서는 Sender와 Receiver 사이에 별도의 Fault Injector를 배치하여 정상 Application 코드를 변경하지 않고 통신 장애를 주입할 수 있도록 구성한다.

```text
Sender
   │
   ▼
Fault Injector
   │
   ▼
Receiver
```

Fault Injector는 다음 장애를 지원한다.

- Transparent Forward
- ACK Drop
- ACK Delay
- DATA Corruption
- Forced Disconnect

이를 통해 Phase 2에서 구현한 Application Reliability 기능을 실제 통신 경로의 장애 상황에서 검증한다.

---

## 2. 배경 개념

### 2.1 TCP Proxy

기존 구조는 Sender와 Receiver가 직접 연결되는 형태였다.

```text
Sender
  │
  │ TCP :5000
  ▼
Receiver
```

Phase 3에서는 중간 Proxy를 추가했다.

```text
Sender
  │
  │ TCP :5000
  ▼
Fault Injector
  │
  │ TCP :5001
  ▼
Receiver
```

Sender는 기존과 동일하게 `127.0.0.1:5000`으로 접속한다.

Fault Injector가 `5000` Port에서 Sender의 연결을 받고, 다시 `127.0.0.1:5001`의 Receiver에 새로운 TCP Connection을 생성한다.

따라서 Fault Injector는 동시에 두 역할을 수행한다.

```text
Sender 관점
Fault Injector = TCP Server

Receiver 관점
Fault Injector = TCP Client
```

실제로는 다음 두 TCP Connection이 존재한다.

```text
Connection #1
Sender ←────────→ Fault Injector

Connection #2
Fault Injector ←────────→ Receiver
```

---

### 2.2 Protocol-aware Proxy

단순 TCP Proxy는 수신한 Byte를 그대로 반대편으로 전달하면 된다.

```text
recv()
  ↓
send()
```

하지만 이번 프로젝트에서는 특정 Message Type과 Sequence를 기준으로 Fault를 주입해야 한다.

예를 들어:

```text
ACK만 Drop
DATA만 Corruption
Heartbeat 수신 시 Disconnect
```

를 수행하려면 Proxy가 현재 전달 중인 Application Packet의 의미를 알아야 한다.

따라서 Fault Injector에서도 Phase 1의 Binary Protocol을 사용하여 Packet을 Decode한다.

```text
TCP Byte Stream
      ↓
Header 수신
      ↓
Payload Length 확인
      ↓
Payload 수신
      ↓
decode_packet()
      ↓
Type / Sequence 확인
```

이를 통해 다음과 같은 로그를 출력할 수 있다.

```text
[Sender -> Receiver] DATA seq=1
[Receiver -> Sender] ACK seq=1
```

---

### 2.3 양방향 Relay와 poll()

Fault Injector는 다음 두 방향을 동시에 처리해야 한다.

```text
Sender → Receiver

Receiver → Sender
```

한 방향에서 `recv()`를 Blocking 상태로 기다리면 반대 방향의 Packet 처리가 지연될 수 있다.

이를 방지하기 위해 `poll()`로 두 Connected Socket을 동시에 감시한다.

```text
poll()
  │
  ├─ sender_fd readable
  │      ↓
  │   Sender Packet 수신
  │      ↓
  │   Receiver로 Forward
  │
  └─ receiver_fd readable
         ↓
      Receiver Packet 수신
         ↓
      Sender로 Forward
```

---

### 2.4 Application Message Drop

이번 프로젝트의 `drop-ack`은 TCP Segment 자체를 손실시키는 것이 아니다.

Receiver Application이 생성한 ACK Packet을 Fault Injector가 수신한 뒤 Sender에게 전달하지 않는 방식이다.

```text
Receiver
   │
   │ ACK
   ▼
Fault Injector
   X
Sender
```

따라서 이를 TCP Packet Loss가 아니라 Application Message Drop으로 구분한다.

---

### 2.5 Corruption과 CRC

Fault Injector가 Decode한 `Packet`의 Payload를 수정한 뒤 다시 `encode_packet()`을 호출하면 변경된 Payload를 기준으로 CRC도 새로 계산된다.

```text
Payload 변경
    ↓
encode_packet()
    ↓
새 Payload에 맞는 CRC 생성
    ↓
Receiver CRC 검증 성공
```

이는 Corruption 테스트가 아니다.

따라서 먼저 정상 Packet을 Encoding한 뒤 CRC 계산이 완료된 Binary Buffer의 Payload만 직접 변경한다.

```text
정상 Packet
    ↓
encode_packet()
    ↓
정상 CRC 생성
    ↓
Payload 1 byte 변경
    ↓
CRC 값은 그대로
    ↓
Receiver CRC mismatch
```

---

## 3. 왜 필요한가

Phase 2에서는 장애를 테스트하기 위해 Receiver 내부에 다음과 같은 임시 로직을 넣었다.

```text
첫 ACK 보내지 않기
첫 HEARTBEAT_ACK 보내지 않기
```

이 방식으로 Reliability 기능 자체는 검증할 수 있었지만 다음 문제가 있다.

```text
장애를 만들기 위해 정상 Application Code를 수정해야 함

장애 시나리오가 Application 구현과 섞임

동일 장애를 반복적으로 재현하기 어려움

실제 통신 경로에서 발생하는 문제와 구조가 다름
```

Fault Injector를 별도 Process로 분리하면 Sender와 Receiver는 정상 코드 상태를 유지할 수 있다.

```text
정상 Sender
     │
     ▼
Fault Injector
     │
     ▼
정상 Receiver
```

장애 주입 정책만 변경하여 여러 상황을 반복적으로 검증할 수 있다.

```text
none
drop-ack
delay-ack
corrupt-data
disconnect
```

---

## 4. 시스템 구조

전체 구조:

```text
Sender
127.0.0.1:5000
      │
      ▼
┌─────────────────────┐
│    Fault Injector   │
│                     │
│  TCP Server :5000   │
│         │           │
│     Packet Decode   │
│         │           │
│    Fault Decision   │
│         │           │
│     Packet Relay    │
│         │           │
│  TCP Client         │
└─────────┬───────────┘
          │
          │ 127.0.0.1:5001
          ▼
       Receiver
```

Fault Mode에 따라 Forward 과정이 달라진다.

```text
none
Packet → Forward

drop-ack
ACK → Drop

delay-ack
ACK → 700ms Delay → Forward

corrupt-data
DATA → Encode → Payload Corruption → Forward

disconnect
HEARTBEAT → Connection Close
```

---

## 5. 구현 과정

### 5.1 Transparent Proxy

먼저 Fault를 전혀 적용하지 않고 Sender와 Receiver 사이에서 모든 Packet을 그대로 Relay하는 구조를 구현했다.

```text
Sender
  │ DATA
  ▼
Fault Injector
  │ DATA
  ▼
Receiver

Receiver
  │ ACK
  ▼
Fault Injector
  │ ACK
  ▼
Sender
```

Fault Injector에서 두 Socket을 `poll()`로 동시에 감시하고 Packet 단위로 Forward한다.

이 단계에서 기존 DATA/ACK와 HEARTBEAT/HEARTBEAT_ACK 통신이 Proxy를 거쳐도 정상적으로 동작하는 것을 확인했다.

---

### 5.2 Fault Mode

실행 인자로 장애 유형을 선택할 수 있도록 구성했다.

```bash
./build/tcp_fault_injector none
./build/tcp_fault_injector drop-ack
./build/tcp_fault_injector delay-ack
./build/tcp_fault_injector corrupt-data
./build/tcp_fault_injector disconnect
```

이를 통해 하나의 Fault Injector 실행파일에서 모든 Phase 3 시나리오를 재현할 수 있다.

---

### 5.3 ACK Drop

`drop-ack` Mode에서는 첫 번째 Application ACK를 Fault Injector가 수신한 뒤 Sender에게 전달하지 않는다.

```text
DATA seq=1
    ↓
Receiver 처리
    ↓
ACK seq=1
    ↓
Fault Injector
    X DROP
```

Sender는 1초 동안 ACK를 받지 못해 Timeout을 발생시키고 같은 Sequence로 DATA를 재전송한다.

```text
ACK Timeout
    ↓
DATA seq=1 Retry
```

Receiver는 이미 `seq=1`을 처리했으므로 Duplicate로 판단한다.

```text
Duplicate DATA seq=1
    ↓
재처리하지 않음
    ↓
ACK 재전송
```

두 번째 ACK는 정상 Forward되어 Sender가 복구한다.

---

### 5.4 ACK Delay

`delay-ack` Mode에서는 첫 번째 ACK를 700ms 지연시킨 뒤 Forward한다.

```text
Receiver ACK
    ↓
Fault Injector
    ↓
700ms Delay
    ↓
Sender
```

Sender의 ACK Timeout은 1000ms이므로:

```text
Injected Delay = 700ms
ACK Timeout    = 1000ms
```

Timeout 이전에 ACK가 도착한다.

따라서 Retry와 Duplicate DATA는 발생하지 않는다.

---

### 5.5 DATA Corruption

`corrupt-data` Mode에서는 첫 DATA Packet을 Encoding한 뒤 Payload 첫 byte를 변경한다.

```cpp
encoded[protocol::kHeaderSize] ^= 0xFF;
```

`kHeaderSize` 이후 첫 Byte는 Payload 시작 위치이다.

```text
Offset 0~15 : Header
Offset 16   : Payload 첫 byte
```

CRC는 Encoding 시점에 이미 계산되었기 때문에 변경되지 않는다.

Receiver가 손상된 Payload 기준으로 CRC를 다시 계산하면 Packet 내부 CRC와 일치하지 않는다.

```text
Stored CRC
≠
Calculated CRC

→ CRC mismatch
```

Receiver는 손상된 DATA를 Application 처리 단계까지 전달하지 않는다.

---

### 5.6 Forced Disconnect

`disconnect` Mode에서는 첫 Heartbeat가 Fault Injector에 도착했을 때 Sender와 Receiver 방향의 Connection을 강제로 종료한다.

```text
HEARTBEAT seq=2
      ↓
Fault Injector
      ↓
[DISCONNECT]
      ↓
Connection Close
```

Sender는 기존 Phase 2에서 Heartbeat Timeout만 Reconnect 대상으로 처리하고 있었다.

Forced Disconnect를 통해 TCP Connection 자체가 종료될 수 있는 경우도 처리해야 한다는 점을 확인했다.

따라서 Sender에 `ConnectionLost` Exception을 추가하여 다음 상황을 별도로 처리했다.

```text
peer disconnected
connection reset
poll connection error
```

Heartbeat 처리 중 Connection Loss가 발생하면:

```text
Connection Lost
    ↓
기존 Socket 종료
    ↓
Reconnect
    ↓
동일 Heartbeat 재전송
```

하도록 보완했다.

---

### 5.7 Fault State 유지

Forced Disconnect가 발생하면 기존 Proxy Connection의 `relay_connection()`이 종료되고 Sender가 새로운 TCP Connection으로 다시 접속한다.

Fault 적용 상태를 Connection 내부에서 관리하면 Reconnect할 때마다 Fault가 다시 발생할 수 있다.

```text
Connection #1
→ Disconnect

Connection #2
→ 다시 Disconnect

Connection #3
→ 다시 Disconnect
```

따라서 Fault 적용 여부를 Connection보다 긴 Fault Injector Process 수준에서 유지했다.

```text
fault_applied = false

첫 Connection
→ Fault 적용
→ fault_applied = true

두 번째 Connection
→ 정상 Forward
```

이를 통해 Forced Disconnect를 한 번 발생시킨 뒤 Reconnect 이후 정상 통신을 검증할 수 있었다.

---

## 6. 핵심 코드

### 양방향 Socket Monitoring

```cpp
pollfd fds[2]{};

fds[0].fd = sender_fd;
fds[0].events = POLLIN;

fds[1].fd = receiver_fd;
fds[1].events = POLLIN;

int result = poll(fds, 2, -1);
```

---

### ACK Drop

```cpp
if (fault_mode == FaultMode::DropAck) {
    std::cout << "[DROP] ACK seq=" << packet.sequence << '\n';
    fault_applied = true;
    continue;
}
```

`continue`를 통해 Sender 방향의 `send_packet()`을 실행하지 않는다.

---

### ACK Delay

```cpp
std::this_thread::sleep_for(
    std::chrono::milliseconds(kAckDelayMs));
```

현재 테스트 값은 700ms이다.

---

### DATA Corruption

```cpp
std::vector<uint8_t> encoded = protocol::encode_packet(packet);
encoded[protocol::kHeaderSize] ^= 0xFF;

send_all(fd, encoded.data(), encoded.size());
```

CRC 생성 이후 Payload를 변경하여 Receiver의 CRC mismatch를 유도한다.

---

### Forced Disconnect

```cpp
shutdown(sender_fd, SHUT_RDWR);
shutdown(receiver_fd, SHUT_RDWR);

throw PeerDisconnected("forced disconnect injected");
```

---

### Sender Connection Loss 처리

```cpp
catch (const ConnectionLost& e) {
    std::cout
        << "Connection lost during HEARTBEAT"
        << " seq=" << heartbeat.sequence
        << " error=" << e.what()
        << '\n';

    reconnect_required = true;
}
```

Timeout뿐만 아니라 실제 Connection Loss도 Reconnect 조건으로 처리한다.

---

## 7. 실행 및 검증

### 7.1 Transparent Proxy

Fault를 적용하지 않은 상태에서 Proxy를 경유한 정상 통신을 검증했다.

![Transparent TCP Proxy](images/phase-3-transparent-proxy.png)

다음 양방향 Packet이 정상적으로 Relay되었다.

```text
DATA → ACK
HEARTBEAT → HEARTBEAT_ACK
```

**Result: PASS**

---

### 7.2 ACK Message Drop

첫 번째 ACK를 Fault Injector에서 Drop했다.

![ACK Drop](images/phase-3-ack-drop.png)

사진에서 다음 흐름을 확인할 수 있다.

```text
Fault Injector
[DROP] ACK seq=1

Sender
ACK timeout seq=1
DATA seq=1 Retry

Receiver
Duplicate DATA seq=1 ignored
ACK seq=1 재전송
```

ACK Drop 이후 Timeout, Retry, Duplicate Detection을 통해 정상 복구했다.

**Result: PASS**

---

### 7.3 ACK Delay

첫 번째 ACK에 700ms Delay를 주입했다.

![ACK Delay](images/phase-3-delay.png)

```text
ACK Delay   = 700ms
ACK Timeout = 1000ms
```

Timeout보다 짧은 지연이므로 Sender는 Retry 없이 ACK를 정상 수신했다.

Receiver에서도 Duplicate DATA가 발생하지 않았다.

**Result: PASS**

---

### 7.4 DATA Corruption

첫 DATA의 Payload를 CRC 계산 이후 변경했다.

![DATA Corruption](images/phase-3-corruption.png)

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

손상된 DATA가 Receiver의 Application 처리 단계까지 전달되지 않고 CRC 검증 단계에서 차단되는 것을 확인했다.

**Result: PASS**

---

### 7.5 Forced Disconnect

첫 Heartbeat가 Fault Injector에 도착한 시점에서 Connection을 강제로 종료했다.

![Forced Disconnect](images/phase-3-disconnect.png)

Fault Injector:

```text
[DISCONNECT] HEARTBEAT seq=2
```

Sender:

```text
Connection lost during HEARTBEAT seq=2
Reconnecting...
Connected to receiver
Resent HEARTBEAT seq=2
Received HEARTBEAT_ACK seq=2
```

Reconnect 이후 `seq=3`, `seq=4` Heartbeat도 정상적으로 처리되었다.

따라서 실제 Connection Loss 이후 통신 복구가 정상적으로 동작함을 확인했다.

**Result: PASS**

---

### 7.6 Normal Regression

모든 Fault 기능을 구현한 뒤 `none` Mode로 정상 회귀 테스트를 수행했다.

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

Fault Injector를 추가하고 Sender의 Connection Loss 처리를 보완한 이후에도 기존 Phase 1~2의 정상 통신이 유지되는 것을 확인했다.

**Result: PASS**

---

### 전체 테스트 결과

| Scenario | 결과 | 판정 |
|---|---|---|
| Transparent Proxy | DATA/ACK 및 Heartbeat 정상 Relay | PASS |
| ACK Drop | Timeout 후 동일 Sequence Retry | PASS |
| Duplicate Detection | Retry DATA 재처리 방지 | PASS |
| ACK Delay | 700ms 지연 후 Timeout 없이 처리 | PASS |
| DATA Corruption | Receiver CRC mismatch 검출 | PASS |
| Forced Disconnect | TCP Connection 강제 종료 감지 | PASS |
| Reconnect | 새 Connection 생성 | PASS |
| Recovery | 동일 Heartbeat 재전송 후 통신 복구 | PASS |
| Normal Regression | `none` Mode 전체 정상 동작 | PASS |

---

## 8. 배운 점

Phase 3에서는 장애 검증 로직을 Application 내부에서 분리하여 독립된 Fault Injector로 구성했다.

이를 통해 다음 구조를 만들었다.

```text
Normal Sender
     ↓
Fault Injector
     ↓
Normal Receiver
```

장애 종류만 실행 옵션으로 변경하여 동일 Application을 다양한 환경에서 반복적으로 검증할 수 있었다.

또한 Fault Injector가 단순 Byte Relay가 아니라 Application Protocol을 이해하는 Protocol-aware Proxy이기 때문에 Message Type과 Sequence를 기준으로 선택적인 Fault를 주입할 수 있었다.

```text
ACK → Drop
ACK → Delay
DATA → Corruption
HEARTBEAT → Disconnect
```

Corruption 구현 과정에서는 Packet 객체를 수정한 뒤 다시 Encoding하면 CRC도 재계산되므로 손상 검증이 이루어지지 않는다는 점을 확인했다.

따라서 CRC 생성 이후의 Wire-format Buffer를 직접 수정하여 실제 전송 중 데이터 손상과 유사한 상태를 재현했다.

Forced Disconnect 테스트에서는 기존 Heartbeat Timeout 처리만으로는 실제 TCP Connection Loss를 복구할 수 없다는 점도 확인했다.

이에 따라 Timeout과 Connection Loss를 구분하면서도 둘 모두 Reconnect 정책으로 연결하도록 Sender를 보완했다.

```text
Heartbeat Timeout
        │
        ├──→ Reconnect
        │
Connection Lost
        │
        └──→ Reconnect
```

현재 Fault Injector는 테스트 목적의 단일 Connection, 단일 Thread 기반 구현이다.

특히 Delay Mode는 `sleep_for()` 동안 Proxy Thread가 Block되므로 범용 Network Emulator로 사용하기에는 한계가 있다.

하지만 이번 프로젝트에서는 복잡한 Traffic Control 시스템을 구현하는 것이 목적이 아니라, 통신 장애를 통제된 조건에서 재현하고 Application Reliability 동작을 검증하는 것이 목적이므로 현재 범위로 제한했다.

다음 Phase에서는 동일 Binary Protocol을 UDP로 확장한다.

TCP와 달리 UDP에서는 Transport Layer가 재전송, 순서 보장, Connection을 제공하지 않으므로 Application에서 필요한 Reliability 기능을 직접 구현하고 TCP 방식과 비교한다.