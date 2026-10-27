# Benchmark c2-base

pcbench-bd-swap, 20 boards, 10,000,000 work units per board, zones refilled before judging, route options `--no-gpu`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 15.0% | 0.0% |
| Mean completion | 86.1% | | |
| Route CPU time, all boards | 5,712 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 3.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 20 | 0.73 | 4 | 157 | 3175 | 6619 | 1.090 | 319 |
| TraceMaker, boards with a reference | 20 | 0.73 | 4 | 157 | 3175 | 6619 | 1.090 | 319 |
| Reference routing | 20 | 1.36 | 38 | 401 | 4837 | 325 | 1.125 | 318 |
