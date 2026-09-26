# CASE 03 — Partial ACK response deadline

## Hypothesis / Problem statement

Partial ACK response deadline 상황에서 기존 reliability mechanism의 조합이 실패할 수 있다는 가설을 production binary로 확인했다.

## Broken invariant

완전한 response 수신까지 하나의 1000ms absolute deadline을 지켜야 한다. partial frame을 소비한 stream을 새 요청의 경계로 재사용하면 안 된다.

## Code suspicion / existing coverage gap

poll() 이후 receive_packet()/recv_exact()가 deadline 없는 blocking recv()로 진행했다. whole-packet delay/drop은 첫 byte만 도착하고 나머지가 정지한 상태를 만들지 않았다.

## Reproduction method

외부 TCP peer는 실제 Sender DATA를 decode한 뒤 80ms에 ACK 첫 byte를 보낸다. A는 3200ms에 나머지를 보낸다. B는 미전송하고 최대 6000ms 관찰한다. 정상 response를 반환하는 Heartbeat 경로도 유지한다.

```bash
python3 reliability_verification/case-03-partial-ack-deadline/reproduce.py --phase after --repeat 3
```

Before는 [suite 실행 방법](../README.md#reproduce)의 v1.0.0 archive를 `--source`로 전달한다. runner의 `--output`으로 재검증 결과 위치를 분리할 수 있다. 원본 소스의 fault 상수를 수정하지 않는다.

## Deterministic PASS / FAIL condition

Before A: 3000ms 초과 후 ACK 성공. Before B: 6000ms까지 무 timeout/retry. After A/B: 900~1250ms 내 명시적 deadline failure, exit 1, peer가 연결 EOF 관찰, ACK 성공/동일 stream retry 없음.

입력 조건과 exit / 실제 protocol event / 후속 동작을 함께 확인한다. 조건이 맞지 않으면 runner가 nonzero로 종료한다. scheduler 허용 범위와 log clock 해석은 suite README를 참고한다.

## Before result

Fresh v1.0.0 execution: **REPRODUCED**, expected observation 6/6. 각 조건 3회. [Structured Before](results/before-summary.json).

## Root cause

tcp/sender.cpp의 poll/receive_packet/recv_exact 경로.

poll() 이후 receive_packet()/recv_exact()가 deadline 없는 blocking recv()로 진행했다. whole-packet delay/drop은 첫 byte만 도착하고 나머지가 정지한 상태를 만들지 않았다.

## Production fix

steady_clock deadline을 header/payload 및 stale skip 전체에 전달한다. 각 poll은 남은 시간만 사용하고 recv(MSG_DONTWAIT)로 무제한 block을 피한다. partial frame timeout은 shutdown/close 후 fail-fast. frame 경계 timeout만 기존 retry/reconnect 경로로 돌아간다.

## After result

Same fault conditions: **PASS**, invariant checks 6/6. 각 조건 3회. [Structured After](results/after-summary.json).

| Variant / phase | Measured outcome ms | Natural exit |
|---|---|---|
| complete-late / Before | 3208.467, 3208.279, 3202.405 | 0, 0, 0 |
| never-complete / Before | no outcome within 6000ms, no outcome within 6000ms, no outcome within 6000ms | None, None, None |
| complete-late / After | 1001.439, 1001.531, 1001.675 | 1, 1, 1 |
| never-complete / After | 1001.427, 1001.603, 1001.503 | 1, 1, 1 |

Before B의 null exit/outcome은 외부 관찰 종료 전 자연 종료가 없었다는 뜻이다. 이후 SIGTERM 정리는 결과 JSON에서 별도로 표시된다. After는 성공 복구가 아니라 framing-safe failure다.

## Regression result

[Phase 1~4 rerun](../regression-summary.json)과 [protocol/deadline policy guards](../guard-summary.json)를 실제 실행했다. 요약 횟수는 [suite README](../README.md#regression-and-policy-checks)에 있다. 기존 sequence/wire format, default 700ms delay를 유지했다.

## Evidence figure

![Measured timeout comparison](../images/case03-timeout-before-after.svg)
