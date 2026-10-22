# Benchmark rel0100-tierB

PCBench tier B, 40 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 70.0% | 50.0% | 12.5% |
| Mean completion | 97.2% | | |
| Route CPU time, all boards | 10,011 s | | |

Both clean: 18; only TraceMaker: 10; only Freerouting: 2.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 40 | 0.75 | 5 | 186 | 2109 | 4996 | 1.132 | 248 |
| TraceMaker, boards with a reference | 40 | 0.75 | 5 | 186 | 2109 | 4996 | 1.132 | 248 |
| Reference routing | 40 | 1.12 | 39 | 366 | 4840 | 938 | 1.138 | 245 |
