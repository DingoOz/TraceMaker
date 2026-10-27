# Benchmark c2q-swap

quick-swap, 10 boards, 1,000,000 work units per board, zones refilled before judging, route options `--no-gpu`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 60.0% | 100.0% | 90.0% |
| Mean completion | 97.7% | | |
| Route CPU time, all boards | 234 s | | |

Both clean: 6; only TraceMaker: 0; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 10 | 0.84 | 0 | 29 | 106 | 261 | 1.038 | 9 |
