| Case | Before | Root cause / fix | After |
|---|---|---|---|
| 01 Late ACK | Heartbeat failure 3/3 | Completed-response matching; one absolute deadline | Exit 0 + Heartbeat 2/3/4 3/3 |
| 02 Partial frame | Partial EOF receiver exits: 6; zero-byte EOF controls survive | EOF stays connection-local; discard incomplete frame | Follow-up DATA/ACK 12/12 |
| 03 Partial ACK | 3208.467, 3208.279, 3202.405 ms ACK success | Deadline-aware receive; partial connection discarded | 1001.439, 1001.531, 1001.675 ms explicit failure |
| 04 UDP stale ACK | ACK mismatch / exit 1 3/3 | Completed-sequence history; fixed deadline | ACK 1/3/2 + exit 0 3/3 |
