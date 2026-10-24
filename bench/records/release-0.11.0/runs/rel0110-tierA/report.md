# Benchmark rel0110-tierA

PCBench tier A, 40 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 100.0% | 100.0% | 87.5% |
| Mean completion | 100.0% | | |
| Route CPU time, all boards | 4,500 s | | |

Both clean: 40; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 40 | 0.82 | 0 | 30 | 450 | 734 | 1.059 | 66 |
| TraceMaker, boards with a reference | 40 | 0.82 | 0 | 30 | 450 | 734 | 1.059 | 66 |
| Reference routing | 40 | 1.20 | 76 | 230 | 2044 | 611 | 1.107 | 90 |
