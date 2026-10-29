# Benchmark rel0120-tierD

PCBench tier D, 22 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 40.9% | 36.4% | 0.0% |
| Mean completion | 95.9% | | |
| Route CPU time, all boards | 12,091 s | | |

Both clean: 4; only TraceMaker: 5; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 22 | 0.62 | 5 | 133 | 4225 | 11265 | 1.095 | 320 |
| TraceMaker, boards with a reference | 22 | 0.62 | 5 | 133 | 4225 | 11265 | 1.095 | 320 |
| Reference routing | 22 | 1.02 | 40 | 663 | 9274 | 1060 | 1.111 | 435 |
