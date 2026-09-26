# CASE 02 — Partial TCP frame disconnect

## Hypothesis / Problem statement

Partial TCP frame disconnect 상황에서 기존 reliability mechanism의 조합이 실패할 수 있다는 가설을 production binary로 확인했다.

## Broken invariant

불완전한 frame은 처리/ACK하지 않아야 하며, 해당 connection의 EOF가 listener 전체를 종료하면 안 된다.

## Code suspicion / existing coverage gap

recv_exact()가 누적 수신량 0이면 PeerDisconnected, 일부 수신 후 EOF이면 runtime_error를 던졌다. 내부 catch는 전자만 처리한다. 기존 forced disconnect는 완전한 Heartbeat를 Proxy가 받은 시점이어서 partial frame 경계가 아니었다.

## Reproduction method

매회 새 Receiver에 직접 접속한다. header 0/8 bytes, header 16 + payload 0/3 of 10 bytes를 보낸 뒤 write-half를 종료하여 EOF를 전달한다. 응답 bytes를 확인하고 새 연결에서 같은 seq=1 정상 DATA를 보낸다.

```bash
python3 reliability_verification/case-02-partial-frame/reproduce.py --phase after --repeat 3
```

Before는 [suite 실행 방법](../README.md#reproduce)의 v1.0.0 archive를 `--source`로 전달한다. runner의 `--output`으로 재검증 결과 위치를 분리할 수 있다. 원본 소스의 fault 상수를 수정하지 않는다.

## Deterministic PASS / FAIL condition

Before: partial header/payload는 exit 1과 connection refusal. 나머지는 생존. After: 네 경계 모두 무 ACK·무 처리, 생존, 같은 sequence의 후속 DATA를 새로 처리하고 CRC-valid ACK 반환.

입력 조건과 exit / 실제 protocol event / 후속 동작을 함께 확인한다. 조건이 맞지 않으면 runner가 nonzero로 종료한다. scheduler 허용 범위와 log clock 해석은 suite README를 참고한다.

## Before result

Fresh v1.0.0 execution: **REPRODUCED**, expected observation 12/12. 각 조건 3회. [Structured Before](results/before-summary.json).

## Root cause

tcp/receiver.cpp의 recv_exact() EOF 분기와 connection 내부 catch.

recv_exact()가 누적 수신량 0이면 PeerDisconnected, 일부 수신 후 EOF이면 runtime_error를 던졌다. 내부 catch는 전자만 처리한다. 기존 forced disconnect는 완전한 Heartbeat를 Proxy가 받은 시점이어서 partial frame 경계가 아니었다.

## Production fix

EOF는 누적 수신량과 무관하게 PeerDisconnected로 분류한다. 기존 connection cleanup/accept loop를 재사용한다. 일반 예외를 포괄적으로 삼키지 않으며 CRC 검증 정책은 유지한다.

## After result

Same fault conditions: **PASS**, invariant checks 12/12. 각 조건 3회. [Structured After](results/after-summary.json).

| Boundary | Before survived / follow-up ACK | After survived / follow-up ACK |
|---|---|---|
| header-zero | 3/3 / 3/3 | 3/3 / 3/3 |
| header-partial | 0/3 / 0/3 | 3/3 / 3/3 |
| payload-zero | 3/3 / 3/3 | 3/3 / 3/3 |
| payload-partial | 0/3 / 0/3 | 3/3 / 3/3 |

## Regression result

[Phase 1~4 rerun](../regression-summary.json)과 [protocol/deadline policy guards](../guard-summary.json)를 실제 실행했다. 요약 횟수는 [suite README](../README.md#regression-and-policy-checks)에 있다. 기존 sequence/wire format, default 700ms delay를 유지했다.

## Evidence figure

![Before](../images/case02-before.svg)

![After](../images/case02-after.svg)
