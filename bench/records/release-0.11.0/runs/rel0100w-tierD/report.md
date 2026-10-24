# Benchmark rel0100w-tierD

PCBench tier D, 22 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 31.8% | 36.4% | 0.0% |
| Mean completion | 94.5% | | |
| Route CPU time, all boards | 9,463 s | | |

Both clean: 3; only TraceMaker: 4; only Freerouting: 5.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 22 | 0.62 | 2 | 170 | 4125 | 9286 | 1.075 | 365 |
| TraceMaker, boards with a reference | 22 | 0.62 | 2 | 170 | 4125 | 9286 | 1.075 | 365 |
| Reference routing | 22 | 1.02 | 40 | 663 | 9274 | 1060 | 1.111 | 435 |
