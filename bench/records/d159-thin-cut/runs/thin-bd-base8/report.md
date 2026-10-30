# Benchmark thin-bd-base8

pcbench-bd-fail, 33 boards, 100,000,000 work units per board, route options `--no-gpu`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 6.1% | 12.1% | 0.0% |
| Mean completion | 93.1% | | |
| Route CPU time, all boards | 64,062 s | | |

Both clean: 0; only TraceMaker: 2; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 33 | 0.72 | 13 | 700 | 8147 | 23591 | 1.116 | 538 |
| TraceMaker, boards with a reference | 33 | 0.72 | 13 | 700 | 8147 | 23591 | 1.116 | 538 |
| Reference routing | 33 | 1.35 | 70 | 911 | 11278 | 4830 | 1.124 | 583 |
