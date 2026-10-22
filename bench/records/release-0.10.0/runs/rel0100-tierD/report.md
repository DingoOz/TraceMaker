# Benchmark rel0100-tierD

PCBench tier D, 22 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 36.4% | 36.4% | 0.0% |
| Mean completion | 95.2% | | |
| Route CPU time, all boards | 12,734 s | | |

Both clean: 3; only TraceMaker: 5; only Freerouting: 5.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 22 | 0.64 | 3 | 178 | 4299 | 9569 | 1.081 | 395 |
| TraceMaker, boards with a reference | 22 | 0.64 | 3 | 178 | 4299 | 9569 | 1.081 | 395 |
| Reference routing | 22 | 1.02 | 40 | 663 | 9274 | 1060 | 1.111 | 435 |
