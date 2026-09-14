# Phase 2 - Application Reliability

## 1. 목표

Phase 1에서 구현한 TCP Binary Protocol 위에 Application 수준의 신뢰성 처리를 추가한다.

TCP 자체는 Byte Stream의 순서 보장과 재전송을 제공하지만, Application 입장에서는 다음 문제들이 여전히 남아 있다.

```text
Receiver Application이 실제로 DATA를 처리했는가?

응답이 오지 않을 경우 Sender는 얼마나 기다려야 하는가?

Timeout 후 동일 DATA를 다시 보내면 중복 처리는 어떻게 막는가?

통신이 없는 동안 상대 Application이 정상 동작 중인지 어떻게 확인하는가?

상대가 응답하지 않을 경우 기존 연결을 어떻게 정리하고 복구하는가?
```

이를 해결하기 위해 다음 기능을 구현했다.

- Application-level ACK
- ACK Timeout
- Retry
- Sequence 기반 Duplicate Detection
- Application Heartbeat
- Heartbeat Timeout
- Disconnect Detection
- TCP Reconnect
- Reconnect 이후 통신 복구

Phase 2의 전체 흐름은 다음과 같다.

```text
DATA
  ↓
ACK 대기
  │
  ├─ ACK 수신
  │      ↓
  │    정상 처리
  │
  └─ Timeout
         ↓
       Retry
         ↓
  동일 Sequence 재전송
         ↓
  Duplicate Detection
```

Heartbeat 경로는 다음과 같다.

```text
HEARTBEAT
    ↓
HEARTBEAT_ACK 대기
    │
    ├─ 응답 수신
    │     ↓
    │   정상 상태
    │
    └─ Timeout
          ↓
    기존 Connection 종료
          ↓
       Reconnect
          ↓
    통신 상태 재확인
```

---

## 2. 배경 개념

### 2.1 TCP ACK와 Application ACK

TCP는 내부적으로 ACK를 사용하여 Byte Stream의 전달 상태를 관리한다.

하지만 TCP ACK는 Application이 DATA를 실제로 처리했다는 의미는 아니다.

이번 프로젝트에서 사용하는 ACK는 Application-level ACK이다.

```text
Sender Application
    ↓
DATA seq=1
    ↓
TCP
    ↓
Receiver Application
    ↓
Protocol Decode
    ↓
DATA 처리
    ↓
ACK seq=1
```

따라서 다음 두 ACK는 의미가 다르다.

| 구분 | 의미 |
|---|---|
| TCP ACK | TCP Transport Layer에서 Byte Stream 전달을 관리 |
| Application ACK | Receiver Application이 해당 DATA를 수신하고 처리했음을 표현 |

이번 프로젝트의 `ACK` Packet은 후자의 역할을 한다.

---

### 2.2 Timeout

Phase 1에서는 Sender가 DATA를 보낸 뒤 바로 `recv()`를 호출했다.

```text
DATA 전송
    ↓
ACK 수신 대기
```

문제는 Receiver가 응답하지 않는 경우 Sender가 계속 대기할 수 있다는 것이다.

이를 해결하기 위해 `poll()`을 사용해 응답 대기 시간을 제한했다.

```text
poll()
  │
  ├─ 응답 있음
  │     ↓
  │   Packet 수신
  │
  └─ Timeout
        ↓
      Retry 판단
```

이번 Phase에서 사용한 정책은 다음과 같다.

```text
ACK Timeout : 1000 ms
Max Retry   : 3회
```

최초 전송까지 포함하면 최대 전송 횟수는 4회이다.

```text
Initial Attempt : 1회
Retry           : 최대 3회
Total Attempts  : 최대 4회
```

이 값은 Protocol 표준값이 아니라 프로젝트에서 정한 테스트 정책이다.

---

### 2.3 Retry와 Sequence Number

Timeout이 발생했을 때 Sender는 동일 DATA를 다시 전송한다.

```text
DATA seq=1
    ↓
Timeout
    ↓
DATA seq=1
```

Retry 시 새로운 Sequence를 생성하지 않는다.

```text
잘못된 예:

DATA seq=1
Timeout
DATA seq=2
```

이렇게 하면 Receiver 입장에서는 새로운 메시지인지 기존 메시지의 재전송인지 구분할 수 없다.

따라서 Retry에서는 동일한 Sequence Number를 유지한다.

---

### 2.4 Duplicate Detection

Application ACK가 전달되지 않았다고 해서 DATA 처리 자체가 실패한 것은 아니다.

예를 들어:

```text
Sender
  │
  │ DATA seq=1
  ▼
Receiver
  │
  │ DATA 처리 완료
  │
  │ ACK seq=1
  X  ACK 유실
```

Sender는 ACK를 받지 못했으므로 Timeout 후 같은 DATA를 재전송한다.

```text
DATA seq=1
```

Receiver가 이를 다시 처리하면 동일 작업이 두 번 실행될 수 있다.

따라서 Receiver는 이미 처리한 Sequence를 저장한다.

```text
processed_sequences = { 1 }
```

같은 Sequence가 다시 들어오면:

```text
DATA seq=1
    ↓
이미 처리한 Sequence
    ↓
실제 처리하지 않음
    ↓
ACK만 다시 전송
```

하도록 구현했다.

---

### 2.5 Heartbeat

DATA 통신이 없는 상태에서도 상대 Application이 실제로 응답 가능한 상태인지 확인할 필요가 있다.

TCP Connection이 존재한다는 사실만으로 상대 Application이 정상 동작 중이라고 판단할 수는 없다.

예를 들어:

```text
TCP Connection 유지

Receiver Application
    ├── Deadlock
    ├── Infinite Loop
    ├── Processing Stall
    └── 응답 불능
```

상태가 발생할 수 있다.

이를 확인하기 위해 Application-level Heartbeat를 사용했다.

```text
Sender
  │
  │ HEARTBEAT
  ▼
Receiver Application
  │
  │ HEARTBEAT_ACK
  ▼
Sender
```

이번 프로젝트에서는 다음 정책을 사용했다.

```text
Heartbeat Interval : 2 sec
Heartbeat Timeout  : 1 sec
Heartbeat Count    : 3
```

---

### 2.6 HEARTBEAT_ACK

`HEARTBEAT_ACK`는 DATA에 대한 `ACK`와 역할이 다르다.

```text
ACK
= DATA가 Application에서 처리되었음을 의미

HEARTBEAT_ACK
= 상대 Application이 현재 요청에 응답 가능한 상태임을 의미
```

Heartbeat에도 Sequence Number를 사용한다.

```text
HEARTBEAT seq=2
    ↓
HEARTBEAT_ACK seq=2
```

Sender는 Message Type과 Sequence를 모두 확인하여 자신이 전송한 Heartbeat에 대한 응답인지 검증한다.

---

### 2.7 Reconnect

Heartbeat Timeout이 발생하면 기존 TCP Connection이 정상적인 통신 경로라고 신뢰하지 않는다.

따라서 기존 Socket을 닫고 새로운 TCP Connection을 만든다.

```text
Old Connection
    ↓
close(fd)
    ↓
socket()
    ↓
connect()
    ↓
New Connection
```

Reconnect는 기존 TCP Connection을 복구하는 것이 아니라 새로운 Connection을 생성하는 것이다.

이번 프로젝트에서는 다음 정책을 사용했다.

```text
Reconnect Delay        : 2 sec
Max Reconnect Attempts : 3
```

---

## 3. 왜 필요한가

Phase 1에서는 다음 통신까지 구현했다.

```text
DATA seq=1
    ↓
Receiver
    ↓
ACK seq=1
```

정상 통신에서는 문제가 없지만 장애 상황을 고려하면 다음 질문이 발생한다.

```text
ACK가 오지 않으면 Sender는 언제까지 기다리는가?

ACK가 오지 않아 DATA를 다시 보내면
Receiver가 같은 작업을 두 번 수행하지 않는가?

DATA 통신이 없는 동안
상대 Application이 살아 있는지는 어떻게 확인하는가?

Heartbeat 응답이 없으면
기존 TCP Connection을 계속 사용해도 되는가?

연결이 비정상이라고 판단한 이후
어떻게 다시 통신을 복구하는가?
```

이를 해결하면서 Phase 1의 단순 Request / Response 구조를 다음과 같이 확장했다.

```text
Binary Protocol
      ↓
Application ACK
      ↓
Timeout
      ↓
Retry
      ↓
Duplicate Detection
      ↓
Heartbeat
      ↓
Heartbeat Timeout
      ↓
Reconnect
```

---

## 4. 시스템 구조

Phase 2의 구조는 다음과 같다.

```text
Sender
  │
  │ DATA seq=N
  ▼
Receiver
  │
  ├─ Sequence 확인
  │
  ├─ 처음 수신
  │    └─ DATA 처리
  │
  ├─ 중복 수신
  │    └─ 재처리하지 않음
  │
  └─ ACK seq=N
       │
       ▼
Sender
```

ACK 응답이 없는 경우:

```text
Sender
  │
  │ DATA seq=N
  ▼
Receiver
  │
  X ACK 응답 없음
  │
Sender
  │
  ├─ poll()
  │
  ├─ Timeout
  │
  └─ DATA seq=N 재전송
             │
             ▼
          Receiver
             │
             ├─ Duplicate Detection
             └─ ACK 재전송
```

Heartbeat 흐름:

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

Heartbeat Timeout 발생 시:

```text
Heartbeat
   ↓
Timeout
   ↓
close(old fd)
   ↓
connect()
   ↓
새 TCP Connection
   ↓
Heartbeat 재전송
   ↓
HEARTBEAT_ACK
   ↓
통신 복구
```

---

## 5. 구현 과정

### 5.1 ACK Timeout

기존 Sender는 ACK를 받을 때까지 바로 `receive_packet()`을 호출했다.

Phase 2에서는 `poll()`을 사용하여 Socket이 읽기 가능한 상태인지 제한 시간 동안 기다린다.

```cpp
pollfd pfd{};
pfd.fd = fd;
pfd.events = POLLIN;

int result = poll(&pfd, 1, timeout_ms);
```

`poll()` 결과는 다음과 같이 처리한다.

```text
result > 0
→ 읽을 데이터 존재

result == 0
→ Timeout

result < 0
→ System Call Error
```

Timeout이 발생하면 `false`를 반환해 상위 Retry 로직이 동작하도록 했다.

---

### 5.2 Response Type / Sequence 검증

DATA ACK와 Heartbeat ACK 모두 동일한 응답 대기 로직을 사용하기 위해 `wait_for_response()`를 구현했다.

```text
wait_for_response(
    expected_type,
    expected_sequence,
    timeout
)
```

DATA의 경우:

```text
Expected Type     = ACK
Expected Sequence = DATA Sequence
```

Heartbeat의 경우:

```text
Expected Type     = HEARTBEAT_ACK
Expected Sequence = HEARTBEAT Sequence
```

따라서 하나의 함수에서 Message Type과 Sequence를 모두 검증한다.

---

### 5.3 Retry

Sender는 DATA를 전송한 뒤 1초 동안 ACK를 기다린다.

```text
DATA seq=1
  ↓
ACK Wait
  ↓
Timeout
  ↓
Retry
```

Retry에서는 동일 Packet을 다시 전송한다.

```text
Attempt 1 : DATA seq=1
Attempt 2 : DATA seq=1
Attempt 3 : DATA seq=1
Attempt 4 : DATA seq=1
```

Sequence Number는 변경하지 않는다.

---

### 5.4 Duplicate Detection

Receiver에서는 이미 처리한 DATA Sequence를 저장한다.

```cpp
std::unordered_set<uint32_t> processed_sequences;
```

Packet 수신 시:

```text
Sequence 존재?
    │
    ├─ NO
    │   ↓
    │ DATA 처리
    │   ↓
    │ Sequence 저장
    │
    └─ YES
        ↓
      Duplicate
        ↓
      재처리하지 않음
```

중복 DATA에도 ACK는 다시 전송한다.

이를 통해 Sender가 ACK를 받지 못해 Retry한 경우에도 Receiver의 실제 작업은 한 번만 실행되도록 했다.

---

### 5.5 Duplicate Detection 검증

Retry 상황을 검증하기 위해 테스트 과정에서는 첫 번째 ACK를 의도적으로 보내지 않았다.

Receiver:

```text
Received DATA seq=1 payload=hello embedded
ACK intentionally suppressed seq=1
Duplicate DATA seq=1 ignored
Sent ACK seq=1
```

Sender:

```text
Sent DATA seq=1 attempt=1 payload=hello embedded
ACK timeout seq=1

Sent DATA seq=1 attempt=2 payload=hello embedded
Received ACK seq=1
```

이를 통해 동일 Sequence의 Retry가 Receiver에서 다시 처리되지 않는 것을 확인했다.

검증 이후 ACK 누락 테스트 코드는 제거했다.

---

### 5.6 Heartbeat

DATA 통신 완료 이후 일정 간격으로 Heartbeat Packet을 전송하도록 구현했다.

```cpp
protocol::Packet heartbeat{
    protocol::MessageType::Heartbeat,
    heartbeat_sequence,
    {}
};
```

Heartbeat는 Payload가 필요하지 않으므로 빈 Payload를 사용한다.

Receiver는 Heartbeat를 수신하면 동일 Sequence를 사용하여 `HEARTBEAT_ACK`를 반환한다.

```text
HEARTBEAT seq=2
    ↓
HEARTBEAT_ACK seq=2
```

---

### 5.7 Heartbeat Timeout

Sender는 Heartbeat를 전송한 뒤 1초 동안 `HEARTBEAT_ACK`를 기다린다.

```text
HEARTBEAT
    ↓
poll(timeout = 1 sec)
    │
    ├─ HEARTBEAT_ACK
    │     ↓
    │   정상
    │
    └─ Timeout
          ↓
       Connection 이상 판단
```

Heartbeat Timeout은 단순 DATA Retry와 다르게 처리했다.

DATA ACK Timeout은 동일 Connection 위에서 Retry하지만, Heartbeat Timeout은 Connection의 상태를 신뢰할 수 없다고 판단하여 Reconnect를 수행한다.

---

### 5.8 Receiver의 Reconnect 지원

기존 Receiver는 하나의 Sender 연결만 처리했다.

Reconnect를 지원하기 위해 `accept()`를 반복 수행하도록 변경했다.

```text
listen()
   ↓
accept()
   ↓
Connection #1 처리
   ↓
Disconnect
   ↓
accept()
   ↓
Connection #2 처리
```

Receiver는 Listening Socket을 유지한 채 Connected Socket만 닫는다.

이를 통해 Sender가 새로운 TCP Connection을 생성하면 다시 `accept()`할 수 있다.

---

### 5.9 Duplicate State 유지

`processed_sequences`는 Connected Socket 처리 루프 밖에 배치했다.

```text
Receiver Process

processed_sequences
        │
        ├─ Connection #1
        │      DATA seq=1
        │
        └─ Connection #2
               DATA seq=1
```

따라서 Reconnect 이후 동일 DATA가 다시 전달되어도 이전 처리 이력을 이용해 중복 여부를 판단할 수 있다.

현재 구현에서는 이 정보가 Receiver Process Memory에만 존재한다.

따라서 Receiver Process 자체가 재시작되면 처리 이력은 사라진다.

---

### 5.10 Heartbeat Timeout 후 Reconnect

Heartbeat Timeout 발생 시 Sender는 기존 Socket을 닫는다.

```cpp
close(fd);
```

이후 `connect_with_retry()`를 호출해 새로운 TCP Connection을 생성한다.

```text
Heartbeat Timeout
     ↓
close(old fd)
     ↓
connect_with_retry()
     ↓
new socket()
     ↓
connect()
```

Reconnect 이후에는 실패했던 동일 Heartbeat를 다시 전송하여 새로운 Connection에서 상대 Application이 응답 가능한지 확인한다.

```text
HEARTBEAT seq=2
    ↓
Timeout
    ↓
Reconnect
    ↓
HEARTBEAT seq=2 재전송
    ↓
HEARTBEAT_ACK seq=2
```

---

## 6. 핵심 코드

### ACK Timeout

```cpp
int result = poll(&pfd, 1, timeout_ms);

if (result == 0) {
    return false;
}
```

---

### Retry

```cpp
for (int attempt = 1; attempt <= kMaxRetries + 1; ++attempt) {
    send_packet(fd, data_packet);

    if (wait_for_response(
            fd,
            protocol::MessageType::Ack,
            data_packet.sequence,
            kAckTimeoutMs)) {

        acknowledged = true;
        break;
    }
}
```

---

### Duplicate Detection

```cpp
bool duplicate =
    processed_sequences.find(packet.sequence)
    != processed_sequences.end();

if (!duplicate) {
    processed_sequences.insert(packet.sequence);
}
```

중복 여부와 관계없이 동일 Sequence의 ACK를 다시 전송한다.

```text
DATA seq=1
    ↓
중복 여부 확인
    ↓
ACK seq=1
```

---

### Heartbeat

```cpp
protocol::Packet heartbeat{
    protocol::MessageType::Heartbeat,
    heartbeat_sequence,
    {}
};
```

Receiver:

```cpp
protocol::Packet heartbeat_ack{
    protocol::MessageType::HeartbeatAck,
    packet.sequence,
    {}
};
```

---

### Reconnect

```cpp
close(fd);

fd = connect_with_retry();

send_packet(fd, heartbeat);
```

기존 Connection을 닫은 뒤 새로운 Socket으로 Connection을 생성한다.

---

## 7. 실행 및 검증

### 7.1 정상 DATA / ACK

Sender:

```text
Connected to receiver attempt=1
Sent DATA seq=1 attempt=1 payload=hello embedded
Received ACK seq=1
```

Receiver:

```text
Sender connected
Received DATA seq=1 payload=hello embedded
Sent ACK seq=1
```

정상적인 경우 Retry 없이 첫 번째 시도에서 DATA / ACK 통신이 완료되었다.

---

### 7.2 ACK Timeout / Retry

테스트를 위해 Receiver의 첫 번째 ACK를 의도적으로 누락했다.

Sender:

```text
Sent DATA seq=1 attempt=1 payload=hello embedded
ACK timeout seq=1

Sent DATA seq=1 attempt=2 payload=hello embedded
Received ACK seq=1
```

동일 Sequence를 사용하여 Retry가 수행되는 것을 확인했다.

---

### 7.3 Duplicate Detection

Receiver:

```text
Received DATA seq=1 payload=hello embedded
ACK intentionally suppressed seq=1
Duplicate DATA seq=1 ignored
Sent ACK seq=1
```

첫 번째 DATA는 실제 처리했지만 Retry된 동일 `seq=1` DATA는 중복으로 판단하여 다시 처리하지 않았다.

---

### 7.4 정상 Heartbeat

Sender:

```text
Sent HEARTBEAT seq=2 count=1
Received HEARTBEAT_ACK seq=2

Sent HEARTBEAT seq=3 count=2
Received HEARTBEAT_ACK seq=3

Sent HEARTBEAT seq=4 count=3
Received HEARTBEAT_ACK seq=4
```

Receiver:

```text
Received HEARTBEAT seq=2
Sent HEARTBEAT_ACK seq=2

Received HEARTBEAT seq=3
Sent HEARTBEAT_ACK seq=3

Received HEARTBEAT seq=4
Sent HEARTBEAT_ACK seq=4
```

Application-level Liveness Check가 정상적으로 동작하는 것을 확인했다.

---

### 7.5 Heartbeat Timeout / Reconnect

Heartbeat Timeout을 재현하기 위해 테스트 과정에서 첫 번째 `HEARTBEAT_ACK`를 의도적으로 보내지 않았다.

Sender:

```text
Sent HEARTBEAT seq=2 count=1
HEARTBEAT timeout seq=2
Reconnecting...
Connected to receiver attempt=1
Resent HEARTBEAT seq=2
Received HEARTBEAT_ACK seq=2
```

Receiver:

```text
Received HEARTBEAT seq=2
HEARTBEAT_ACK intentionally suppressed seq=2

Sender disconnected
Waiting for reconnect

Sender connected
Received HEARTBEAT seq=2
Sent HEARTBEAT_ACK seq=2
```

Reconnect 이후에도 다음 Heartbeat가 정상적으로 이어지는 것을 확인했다.

```text
HEARTBEAT seq=3
→ HEARTBEAT_ACK seq=3

HEARTBEAT seq=4
→ HEARTBEAT_ACK seq=4
```

검증 이후 `HEARTBEAT_ACK` 누락 테스트 코드는 제거했다.

---

### 7.6 최종 정상 회귀 테스트

테스트용 장애 코드를 모두 제거한 뒤 정상 동작을 다시 확인했다.

Sender:

```text
Connected to receiver attempt=1
Sent DATA seq=1 attempt=1 payload=hello embedded
Received ACK seq=1
Sent HEARTBEAT seq=2 count=1
Received HEARTBEAT_ACK seq=2
Sent HEARTBEAT seq=3 count=2
Received HEARTBEAT_ACK seq=3
Sent HEARTBEAT seq=4 count=3
Received HEARTBEAT_ACK seq=4
```

Receiver:

```text
Receiver listening on port 5000
Sender connected
Received DATA seq=1 payload=hello embedded
Sent ACK seq=1
Received HEARTBEAT seq=2
Sent HEARTBEAT_ACK seq=2
Received HEARTBEAT seq=3
Sent HEARTBEAT_ACK seq=3
Received HEARTBEAT seq=4
Sent HEARTBEAT_ACK seq=4
Sender disconnected
Waiting for connection
```

테스트 결과:

| Scenario | 결과 | 판정 |
|---|---|---|
| Normal DATA / ACK | 최초 전송에서 ACK 수신 | PASS |
| ACK Timeout | 1초 후 Timeout 검출 | PASS |
| Retry | 동일 Sequence로 재전송 | PASS |
| Duplicate DATA | 재처리하지 않고 ACK만 반환 | PASS |
| Normal Heartbeat | HEARTBEAT / HEARTBEAT_ACK 정상 교환 | PASS |
| Heartbeat Timeout | 응답 없음 검출 | PASS |
| Disconnect | 기존 Connection 종료 | PASS |
| Reconnect | 새로운 TCP Connection 생성 | PASS |
| Recovery | Reconnect 후 Heartbeat 정상 응답 | PASS |
| Normal Regression | 장애 코드 제거 후 전체 정상 동작 | PASS |

---

## 8. 배운 점

Phase 1에서는 TCP 연결과 Binary Protocol 자체에 집중했다.

Phase 2에서는 TCP를 사용하더라도 Application 수준에서는 별도의 상태 관리가 필요하다는 점을 확인했다.

특히 다음 차이를 이해할 수 있었다.

```text
TCP Reliability
= Byte Stream 전달을 위한 Transport Layer 기능

Application Reliability
= 실제 메시지 처리 여부와 서비스 상태를 판단하기 위한 정책
```

Application ACK를 추가하면서 TCP 내부 ACK와 Application 처리 완료의 의미가 다르다는 점을 확인했다.

또한 Timeout과 Retry를 구현하면서 단순 재전송만으로는 충분하지 않다는 점도 확인했다.

```text
Retry
    ↓
동일 DATA 재전송 가능
    ↓
Duplicate 발생 가능
    ↓
Sequence 기반 Deduplication 필요
```

이는 메시지 기반 시스템에서 발생하는 재전달과 중복 처리 문제와 유사한 구조이다.

Heartbeat 구현을 통해 TCP Connection이 존재하는 것과 상대 Application이 실제로 응답 가능한 상태인 것도 구분할 필요가 있다는 점을 확인했다.

```text
Connection 존재
≠
Application 정상
```

따라서:

```text
HEARTBEAT
    ↓
HEARTBEAT_ACK
```

를 Application-level Liveness Check로 사용했다.

Heartbeat 응답이 없는 경우에는 기존 Connection을 계속 신뢰하지 않고 Socket을 닫은 뒤 새로운 TCP Connection을 생성하도록 구현했다.

```text
Heartbeat Timeout
    ↓
Disconnect
    ↓
Reconnect
    ↓
Liveness 재확인
```

또한 Duplicate Detection 상태를 개별 TCP Connection이 아닌 Receiver Process 수준에서 유지함으로써 Reconnect 이후에도 같은 Sequence의 재처리를 방지하도록 했다.

현재 구현의 한계도 존재한다.

`processed_sequences`는 Memory에만 저장되므로 Receiver Process가 재시작되면 처리 이력이 사라진다.

또한 새로운 논리 세션이 시작되어 Sequence가 다시 `1`부터 시작될 경우 기존 Sequence와 충돌할 수 있다.

이를 해결하려면 향후 다음과 같은 방법을 고려할 수 있다.

```text
Session ID
Persistent Deduplication State
Sequence Scope 관리
```

하지만 이번 프로젝트에서는 하나의 논리 통신 세션 내에서 Timeout, Retry, Duplicate Detection, Heartbeat, Reconnect의 동작을 직접 구현하고 검증하는 데 범위를 제한했다.

다음 Phase에서는 별도의 Fault Injector를 추가하여 Sender와 Receiver 사이에서 장애를 직접 주입한다.

```text
Sender
   ↓
Fault Injector
   ├── Message Drop
   ├── Delay
   ├── Corruption
   └── Disconnect
   ↓
Receiver
```

이를 통해 지금까지 테스트 코드로 직접 발생시킨 장애를 통신 경로 외부에서 재현하고 신뢰성 로직을 검증한다.