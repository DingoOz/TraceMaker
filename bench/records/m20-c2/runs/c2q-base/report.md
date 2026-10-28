# Benchmark c2q-base

quick-swap, 10 boards, 1,000,000 work units per board, zones refilled before judging, route options `--no-gpu`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 70.0% | 100.0% | 90.0% |
| Mean completion | 97.7% | | |
| Route CPU time, all boards | 227 s | | |

Both clean: 7; only TraceMaker: 0; only Freerouting: 3.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 10 | 0.87 | 0 | 30 | 113 | 260 | 1.044 | 11 |
| TraceMaker, boards with a reference | 10 | 0.87 | 0 | 30 | 113 | 260 | 1.044 | 11 |
| Reference routing | 10 | 1.45 | 2 | 22 | 261 | 19 | 1.120 | 32 |
