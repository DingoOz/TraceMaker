# Benchmark rel0110-tierB

PCBench tier B, 40 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 70.0% | 50.0% | 12.5% |
| Mean completion | 97.1% | | |
| Route CPU time, all boards | 12,409 s | | |

Both clean: 18; only TraceMaker: 10; only Freerouting: 2.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 40 | 0.74 | 5 | 196 | 2101 | 4978 | 1.127 | 239 |
| TraceMaker, boards with a reference | 40 | 0.74 | 5 | 196 | 2101 | 4978 | 1.127 | 239 |
| Reference routing | 40 | 1.12 | 39 | 366 | 4840 | 938 | 1.138 | 245 |
