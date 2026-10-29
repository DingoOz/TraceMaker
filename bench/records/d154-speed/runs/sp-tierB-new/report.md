# Benchmark sp-tierB-new

PCBench tier B, 40 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 72.5% | 50.0% | 12.5% |
| Mean completion | 97.5% | | |
| Route CPU time, all boards | 16,036 s | | |

Both clean: 19; only TraceMaker: 10; only Freerouting: 1.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 40 | 0.77 | 9 | 216 | 2080 | 5015 | 1.122 | 235 |
| TraceMaker, boards with a reference | 40 | 0.77 | 9 | 216 | 2080 | 5015 | 1.122 | 235 |
| Reference routing | 40 | 1.12 | 39 | 366 | 4840 | 938 | 1.138 | 245 |
