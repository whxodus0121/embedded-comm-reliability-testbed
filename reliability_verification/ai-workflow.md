# AI-Assisted Verification Workflow

이 문서는 기존 테스트베드에 AI를 어떻게 활용했으며, 어떤 실행 근거로 제안을 채택하거나 제외했는지 설명한다. AI의 출력 자체는 결함이나 수정 성공의 증거가 아니다.

## 1. 왜 AI를 사용했는가

사람이 설계한 Phase 1~4는 ACK drop/delay, corruption, disconnect, retry, duplicate detection, out-of-order 등 대표 fault scenario를 검증했다. 그러나 이 검증만으로 timeout 경계, 부분 수신, 과거 응답과 후속 요청의 조합까지 보장하지는 않는다.

AI는 기존 coverage 밖의 상태 조합을 검토하고 실행으로 확인할 failure hypothesis를 제안하는 데 사용했다. 이미 문서화된 limitation은 새로운 결함으로 집계하지 않았다.

## 2. AI에 제공한 입력

분석 대상은 v1.0.0 source와 저장소에 보존된 설계·검증 자료였다.

- TCP/UDP Sender·Receiver, 공통 protocol codec/CRC, TCP fault injector 구현
- [프로젝트 README](../README.md)의 protocol behavior, fault 범위, validation 및 limitation
- 기존 Phase 1~4의 설계·검증 맥락: [Phase 1](../docs/phase-1.md), [Phase 2](../docs/phase-2.md), [Phase 3](../docs/phase-3.md), [Phase 4](../docs/phase-4.md)
- 사용자가 지정한 후보, 변경 금지 단계, 재현·수정 범위와 완료 조건

실험 이후에는 실제 로그, exit code와 structured result를 근거로 가설과 수정 결과를 재평가했다. 운영 트래픽이나 운영 장애 데이터에 기반한 결과는 아니다.

## 3. AI의 역할

AI는 source/protocol/coverage 분석, 누락된 failure hypothesis와 코드 경로 제안, reproduction 조건 설계, verification runner와 visualization 구현, production fix 및 regression workflow 구성에 참여했다. 사용자가 범위와 검증 조건을 지정했고, AI는 그 범위에서 코드 조사와 도구 실행을 수행했다.

실제 bug 여부, defect classification, root cause, PASS/FAIL, fix 성공 여부는 자연어 제안을 그대로 채택하지 않았다. 코드의 예외·상태 전파 경로를 조사하고, 동일 fault 입력의 Before/After에서 관측된 동작을 명시적 assertion과 비교했다. 이는 별도의 독립적인 사람 코드 리뷰나 포괄적인 정확성 증명을 수행했다는 주장은 아니다.

## 4. Deterministic Validation

**AI-assisted hypothesis generation → controlled reproduction → deterministic validation → root cause analysis → fix → regression verification**

가설의 판정에는 다음 observable을 사용했다.

- 실제 C++ production binary의 stdout/stderr 및 자연 종료 exit code
- monotonic clock으로 기록한 elapsed time, timeout/retry 횟수
- 실제 packet의 ACK, type, sequence 및 duplicate detection
- Receiver 생존뿐 아니라 새 연결에서 후속 DATA 처리와 ACK 반환
- 같은 입력의 반복 재현, 수정 후 동일 실험과 기존 회귀 시나리오

[공통 runner](common/runner.py)의 assertion이 기대 동작을 검사한다. Before의 `matched_expected_behavior`는 결함 또는 대조 조건의 재현을 뜻하며 정상 동작 PASS와 혼동하지 않는다. 외부 정리로 종료한 프로세스는 자연 종료와 구분한다.

CASE 01의 실험용 ACK delay 1200ms와 UDP relay용 port 변경은 repository 밖 build copy에만 적용했다. production ACK delay는 700ms다. CASE 03은 제어된 TCP peer를 사용했다. [재현 방법과 범위](README.md#reproduce)에 구체적인 명령과 차이를 보존했다.

여기서 deterministic은 입력과 판정 조건을 명시했다는 뜻이다. OS scheduling까지 동일하거나 모든 실행 시간이 같다는 뜻은 아니다. CASE 03의 1000ms deadline은 900~1250ms 관찰 허용 범위로 검증했고, process 간 로그 순서를 packet capture의 미세한 순서로 해석하지 않았다.

## 5. 실제 사례

| Case | 가설과 실제 관찰 | 수정 후 검증 |
|---|---|---|
| [01 Late ACK](case-01-late-ack/README.md) | timeout/retry 뒤 남은 ACK가 후속 Heartbeat를 실패시킴, 3/3 | 완료 응답만 stale로 소비; Heartbeat 2/3/4 성공, 3/3 |
| [02 Partial frame](case-02-partial-frame/README.md) | partial EOF가 Receiver 전체 종료로 확대, partial 조건 6/6 | EOF를 connection-local로 처리; 네 경계 × 3회 후속 DATA/ACK 성공, 12/12 |
| [03 Partial ACK](case-03-partial-ack-deadline/README.md) | 최초 readable 이후 blocking receive가 timeout을 초과 | 전체 응답 deadline 적용; partial connection 폐기, 6/6 |
| [04 UDP stale ACK](case-04-udp-stale-ack/README.md) | 이전 ACK가 현재 sequence mismatch를 일으킴, 3/3 | 완료 sequence 이력으로 분류; ACK 1/3/2 성공, 3/3 |

대표 사례인 CASE 03에서는 최초 `poll()` 이후 `recv_exact()`에 remaining deadline이 없다는 가설을 세웠다. ACK 첫 byte를 80ms에 보내고 나머지를 3200ms에 보내는 실험으로 확인했다. 설정 timeout은 **1000ms**다.

| Run | Before: 지연 ACK 성공 시각 (ms) | After: bounded explicit failure 시각 (ms) |
|---|---:|---:|
| 1 | 3208.467 | 1001.439 |
| 2 | 3208.279 | 1001.531 |
| 3 | 3202.405 | 1001.675 |

출처: [Before JSON](case-03-partial-ack-deadline/results/before-summary.json), [After JSON](case-03-partial-ack-deadline/results/after-summary.json). 이 값은 새 측정이나 성능 개선율이 아니라 보존된 동일 입력 실험의 결과다.

**After는 ACK 성공이 아니다.** 부분 frame 때문에 안전하게 재사용할 수 없는 TCP connection을 약 1초 absolute deadline에서 폐기하고 exit 1로 종료하는 bounded failure다. 자동 DATA reconnect/replay는 추가하지 않았다. 나머지 ACK를 보내지 않은 별도 조건은 Before에서 3/3 모두 6000ms 관찰 종료까지 응답 처리가 완료되지 않았고, After에서는 명시적으로 실패했다.

## 6. AI 출력을 reject한 사례

CASE 05에서는 UDP Receiver가 실제 ACK를 전송하고 exit 0한 뒤 relay가 ACK를 버리면 Sender의 retry가 실패하는 현상을 3회 재현했다. [관측 결과](candidate05-summary.json)는 보존했다.

그러나 Receiver가 지속적인 recovery service인지, single-message demo인지 lifecycle contract가 명확하지 않았다. 현상은 확인했지만 확정 결함이라는 해석은 채택하지 않았다.

**Reproduced phenomenon ≠ confirmed defect.** CASE 05는 **Contract unclear**이며 main defect count와 production fix에서 제외했다. 최종 분류는 **5개 가설 조사, CASE 01~04의 4개 main defect 재현·수정, 1개 계약 불명확 후보 제외**다. AI가 제안한 모든 문제를 bug로 채택한 것이 아니다.

## 7. 최종 역할 분리

| 단계 | AI의 역할 | 판정 근거 / engineering judgment |
|---|---|---|
| Coverage analysis | 코드·문서 분석 보조 | 기존 검증과 limitation 대조 |
| Hypothesis generation | 미검증 조합과 경로 제안 | 요청 범위와 invariant에 따라 선택 |
| Reproduction design | runner 구현 및 도구 실행 | 제어된 입력, 실제 binary와 반복 관측 |
| Defect verdict | 증거 해석 보조 | 실행 증거와 설계 계약; CASE 05 제외 |
| Timing / exit / ACK measurement | 수집 코드 작성·실행 | monotonic clock, OS exit, 실제 packet |
| Fix implementation | 구현 보조 | 원인 코드 검토와 동일 입력 After assertion |
| Regression verdict | 검증 구성·실행 보조 | 기존 시나리오와 정책 guard assertion |

보존된 결과는 [기존 regression 8/8 PASS](regression-summary.json), [추가 policy guards 11/11 PASS](guard-summary.json)다. PASS는 이 입력과 assertion의 범위에 한정한다. AI가 작성한 harness도 불완전할 수 있으므로 source, 입력, assertion, JSON과 [visualization source mapping](images/sources.json)을 함께 공개하여 검토와 재실행이 가능하게 했다.

추가 한계와 이번 범위 밖 항목은 [Remaining limits](README.md#remaining-limits--observations-outside-this-change)에 정리했다.
