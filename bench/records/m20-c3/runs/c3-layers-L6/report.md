# Benchmark c3-layers-L6

PCBench tier A, 14 boards, 10,000,000 work units per board, zones refilled before judging.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 7.1% | 14.3% | 0.0% |
| Mean completion | 89.1% | | |
| Route CPU time, all boards | 3,436 s | | |

Both clean: 0; only TraceMaker: 1; only Freerouting: 2.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 14 | 0.57 | 4 | 89 | 1762 | 2472 | 1.069 | 150 |
