# Benchmark real-bd-relax

pcbench-bd-fail, 33 boards, 100,000,000 work units per board, route options `--no-gpu --relax --round-work 20000000 --rounds 5 --no-rescue`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 15.2% | 12.1% | 0.0% |
| Mean completion | 91.7% | | |
| Route CPU time, all boards | 67,272 s | | |

Both clean: 0; only TraceMaker: 5; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 33 | 0.94 | 36 | 126 | 3773 | 13349 | 1.130 | 615 |
| TraceMaker, boards with a reference | 33 | 0.94 | 36 | 126 | 3773 | 13349 | 1.130 | 615 |
| Reference routing | 33 | 1.35 | 70 | 911 | 11278 | 4830 | 1.124 | 583 |
