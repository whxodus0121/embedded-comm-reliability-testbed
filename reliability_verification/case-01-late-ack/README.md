# CASE 01 — Late ACK / Retry / stale response

## Hypothesis / Problem statement

Late ACK / Retry / stale response 상황에서 기존 reliability mechanism의 조합이 실패할 수 있다는 가설을 production binary로 확인했다.

## Broken invariant

완료된 DATA의 정상 ACK가 후속 Heartbeat 요청을 실패시키면 안 된다.

## Code suspicion / existing coverage gap

wait_for_response()가 첫 packet의 type/sequence가 다르면 즉시 예외를 던졌다. 기존 ACK drop은 첫 ACK를 제거하고 700ms delay는 retry를 만들지 않아 ACK 두 개가 남는 조합을 놓쳤다.

## Reproduction method

실제 Receiver와 1200ms Proxy copy를 시작하고 원본 Sender를 실행한다. timeout/retry 1회, duplicate 1회, Receiver/Proxy ACK 두 번을 먼저 확인한다.

```bash
python3 reliability_verification/case-01-late-ack/reproduce.py --phase after --repeat 3
```

Before는 [suite 실행 방법](../README.md#reproduce)의 v1.0.0 archive를 `--source`로 전달한다. runner의 `--output`으로 재검증 결과 위치를 분리할 수 있다. 원본 소스의 fault 상수를 수정하지 않는다.

## Deterministic PASS / FAIL condition

Before: Sender exit 1 + unexpected response type. After: 동일 fault 조건에서 Sender exit 0 및 Heartbeat ACK 2/3/4 모두 수신.

입력 조건과 exit / 실제 protocol event / 후속 동작을 함께 확인한다. 조건이 맞지 않으면 runner가 nonzero로 종료한다. scheduler 허용 범위와 log clock 해석은 suite README를 참고한다.

## Before result

Fresh v1.0.0 execution: **REPRODUCED**, expected observation 3/3. 각 조건 3회. [Structured Before](results/before-summary.json).

## Root cause

tcp/sender.cpp의 wait_for_response().

wait_for_response()가 첫 packet의 type/sequence가 다르면 즉시 예외를 던졌다. 기존 ACK drop은 첫 ACK를 제거하고 700ms delay는 retry를 만들지 않아 ACK 두 개가 남는 조합을 놓쳤다.

## Production fix

실제로 완료한 (response type, sequence) 쌍만 stale로 소비한다. CRC를 검증하고 ACK/HB_ACK 타입 및 empty payload를 요구한다. 아직 완료하지 않은 response는 오류다. 단일 absolute deadline을 반복해서 사용한다.

## After result

Same fault conditions: **PASS**, invariant checks 3/3. 각 조건 3회. [Structured After](results/after-summary.json).

| Phase | Sender exits | Retry counts |
|---|---|---|
| Before | 1, 1, 1 | 1, 1, 1 |
| After | 0, 0, 0 | 1, 1, 1 |

## Regression result

[Phase 1~4 rerun](../regression-summary.json)과 [protocol/deadline policy guards](../guard-summary.json)를 실제 실행했다. 요약 횟수는 [suite README](../README.md#regression-and-policy-checks)에 있다. 기존 sequence/wire format, default 700ms delay를 유지했다.

## Evidence figure

![Before](../images/case01-before.svg)

![After](../images/case01-after.svg)
