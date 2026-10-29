# Benchmark rel0120-tierC

PCBench tier C, 30 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 50.0% | 46.7% | 16.7% |
| Mean completion | 97.5% | | |
| Route CPU time, all boards | 14,029 s | | |

Both clean: 11; only TraceMaker: 4; only Freerouting: 3.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.69 | 3 | 165 | 3162 | 8230 | 1.091 | 362 |
| TraceMaker, boards with a reference | 30 | 0.69 | 3 | 165 | 3162 | 8230 | 1.091 | 362 |
| Reference routing | 30 | 1.27 | 61 | 579 | 6586 | 4042 | 1.112 | 405 |
