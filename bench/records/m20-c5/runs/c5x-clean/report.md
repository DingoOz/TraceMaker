# Benchmark c5x-clean

pcbench-bd-clean, 59 boards, 10,000,000 work units per board, zones refilled before judging, route options `--no-gpu`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 78.0% | 64.4% | 17.0% |
| Mean completion | 99.8% | | |
| Route CPU time, all boards | 10,483 s | | |

Both clean: 28; only TraceMaker: 18; only Freerouting: 10.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 59 | 0.58 | 7 | 123 | 3466 | 6993 | 1.064 | 301 |
| TraceMaker, boards with a reference | 59 | 0.58 | 7 | 123 | 3466 | 6993 | 1.064 | 301 |
| Reference routing | 59 | 0.85 | 70 | 697 | 9422 | 1210 | 1.101 | 502 |
