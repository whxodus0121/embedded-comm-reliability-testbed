# CASE 04 — UDP stale ACK contamination

## Hypothesis / Problem statement

UDP stale ACK contamination 상황에서 기존 reliability mechanism의 조합이 실패할 수 있다는 가설을 production binary로 확인했다.

## Broken invariant

동일 session의 이미 완료된 요청 ACK가 현재 DATA를 실패시키거나 timeout을 연장하면 안 된다.

## Code suspicion / existing coverage gap

wait_for_ack()가 하나의 packet을 읽고 sequence mismatch로 종료했다. ACK drop과 1→3→2 검증은 분리되어 이전 ACK가 다음 대기 구간에 들어오는 상황을 만들지 않았다.

## Reproduction method

실제 Sender out-of-order와 port만 바꾼 Receiver copy 사이에 relay를 둔다. 첫 ACK1 보류 → retry의 ACK1 전달 → DATA3 관찰 → 보류 ACK1 전달. 실제 ACK3은 CRC 확인 후 150ms 뒤 전달한다.

```bash
python3 reliability_verification/case-04-udp-stale-ack/reproduce.py --phase after --repeat 3
```

Before는 [suite 실행 방법](../README.md#reproduce)의 v1.0.0 archive를 `--source`로 전달한다. runner의 `--output`으로 재검증 결과 위치를 분리할 수 있다. 원본 소스의 fault 상수를 수정하지 않는다.

## Deterministic PASS / FAIL condition

Before: seq1 성공 후 seq3에서 mismatch/exit1. After: 같은 timeout/retry/duplicate 조건에서 ACK1/3/2 모두 성공하고 양쪽 exit0. 실제 ACK3 생성 여부와 같은 sender endpoint를 확인한다.

입력 조건과 exit / 실제 protocol event / 후속 동작을 함께 확인한다. 조건이 맞지 않으면 runner가 nonzero로 종료한다. scheduler 허용 범위와 log clock 해석은 suite README를 참고한다.

## Before result

Fresh v1.0.0 execution: **REPRODUCED**, expected observation 3/3. 각 조건 3회. [Structured Before](results/before-summary.json).

## Root cause

udp/sender.cpp의 wait_for_ack()와 완료 이력 전달.

wait_for_ack()가 하나의 packet을 읽고 sequence mismatch로 종료했다. ACK drop과 1→3→2 검증은 분리되어 이전 ACK가 다음 대기 구간에 들어오는 상황을 만들지 않았다.

## Production fix

완료 sequence set을 사용한다. 단순 수치 비교는 1→3→2에서 stale ACK3을 오판할 수 있다. empty valid ACK만 분류하며 unknown sequence/type/CRC 오류는 거부한다. stale skip과 EINTR에서도 deadline은 갱신하지 않는다.

## After result

Same fault conditions: **PASS**, invariant checks 3/3. 각 조건 3회. [Structured After](results/after-summary.json).

| Phase | Sender exits | Retry counts |
|---|---|---|
| Before | 1, 1, 1 | 1, 1, 1 |
| After | 0, 0, 0 | 1, 1, 1 |

## Regression result

[Phase 1~4 rerun](../regression-summary.json)과 [protocol/deadline policy guards](../guard-summary.json)를 실제 실행했다. 요약 횟수는 [suite README](../README.md#regression-and-policy-checks)에 있다. 기존 sequence/wire format, default 700ms delay를 유지했다.

## Evidence figure

![Before](../images/case04-before.svg)

![After](../images/case04-after.svg)
