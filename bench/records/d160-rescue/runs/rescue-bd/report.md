# Benchmark rescue-bd

pcbench-bd-fail, 33 boards, 100,000,000 work units per board, route options `--no-gpu`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 15.2% | 12.1% | 0.0% |
| Mean completion | 93.4% | | |
| Route CPU time, all boards | 110,958 s | | |

Both clean: 0; only TraceMaker: 5; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 33 | 0.81 | 15 | 529 | 6729 | 19469 | 1.119 | 548 |
| TraceMaker, boards with a reference | 33 | 0.81 | 15 | 529 | 6729 | 19469 | 1.119 | 548 |
| Reference routing | 33 | 1.35 | 70 | 911 | 11278 | 4830 | 1.124 | 583 |
