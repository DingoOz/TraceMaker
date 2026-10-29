# Benchmark sp-tierC-base

PCBench tier C, 30 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 53.3% | 46.7% | 16.7% |
| Mean completion | 97.5% | | |
| Route CPU time, all boards | 19,254 s | | |

Both clean: 11; only TraceMaker: 5; only Freerouting: 3.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.68 | 6 | 240 | 3622 | 8980 | 1.097 | 306 |
| TraceMaker, boards with a reference | 30 | 0.68 | 6 | 240 | 3622 | 8980 | 1.097 | 306 |
| Reference routing | 30 | 1.27 | 61 | 579 | 6586 | 4042 | 1.112 | 405 |
