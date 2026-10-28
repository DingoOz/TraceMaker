# Benchmark b1-profiles2

b1-profiles, 4 boards, 3,000,000 work units per board, zones refilled before judging, route options `--soft-zones`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 0.0% | 0.0% |
| Mean completion | 84.2% | | |
| Route CPU time, all boards | 434 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 4 | 0.81 | 0 | 7 | 232 | 240 | 1.108 | 20 |
