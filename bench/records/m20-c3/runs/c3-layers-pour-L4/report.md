# Benchmark c3-layers-pour-L4

PCBench tier A, 14 boards, 10,000,000 work units per board, zones refilled before judging, route options `--no-gpu --pour auto`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 7.1% | 14.3% | 0.0% |
| Mean completion | 90.4% | | |
| Route CPU time, all boards | 2,242 s | | |

Both clean: 0; only TraceMaker: 1; only Freerouting: 2.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 14 | 0.72 | 6 | 142 | 1285 | 1993 | 1.069 | 64 |
