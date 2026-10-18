# Benchmark rel0100-tierC

PCBench tier C, 30 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 43.3% | 46.7% | 16.7% |
| Mean completion | 97.3% | | |
| Route CPU time, all boards | 12,483 s | | |

Both clean: 10; only TraceMaker: 3; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.69 | 11 | 300 | 3694 | 9932 | 1.112 | 412 |
| TraceMaker, boards with a reference | 30 | 0.69 | 11 | 300 | 3694 | 9932 | 1.112 | 412 |
| Reference routing | 30 | 1.27 | 61 | 579 | 6586 | 4042 | 1.112 | 405 |
