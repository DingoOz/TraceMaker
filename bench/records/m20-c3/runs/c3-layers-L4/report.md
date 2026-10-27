# Benchmark c3-layers-L4

PCBench tier A, 14 boards, 10,000,000 work units per board, zones refilled before judging.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 7.1% | 14.3% | 0.0% |
| Mean completion | 90.1% | | |
| Route CPU time, all boards | 2,771 s | | |

Both clean: 0; only TraceMaker: 1; only Freerouting: 2.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 14 | 0.61 | 3 | 76 | 1288 | 2340 | 1.078 | 159 |
