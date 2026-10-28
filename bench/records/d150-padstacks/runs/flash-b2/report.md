# Benchmark flash-b2

b2-delay, 3 boards, 3,000,000 work units per board, zones refilled before judging, route options `--soft-zones`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 0.0% | 0.0% |
| Mean completion | 79.6% | | |
| Route CPU time, all boards | 286 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 2 ({'starved_thermal': 2}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 3 | 1.40 | 0 | 21 | 83 | 136 | 1.135 | 20 |
