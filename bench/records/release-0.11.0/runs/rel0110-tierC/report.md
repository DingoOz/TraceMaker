# Benchmark rel0110-tierC

PCBench tier C, 30 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 46.7% | 46.7% | 16.7% |
| Mean completion | 97.4% | | |
| Route CPU time, all boards | 14,872 s | | |

Both clean: 10; only TraceMaker: 4; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.71 | 17 | 280 | 3799 | 8899 | 1.088 | 330 |
| TraceMaker, boards with a reference | 30 | 0.71 | 17 | 280 | 3799 | 8899 | 1.088 | 330 |
| Reference routing | 30 | 1.27 | 61 | 579 | 6586 | 4042 | 1.112 | 405 |
