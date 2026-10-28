# Benchmark c5x-fail-txn

pcbench-bd-fail, 33 boards, 10,000,000 work units per board, zones refilled before judging, route options `--no-gpu --txn 1`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 12.1% | 0.0% |
| Mean completion | 90.1% | | |
| Route CPU time, all boards | 7,346 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 33 | 0.82 | 7 | 143 | 3668 | 7797 | 1.097 | 581 |
| TraceMaker, boards with a reference | 33 | 0.82 | 7 | 143 | 3668 | 7797 | 1.097 | 581 |
| Reference routing | 33 | 1.35 | 70 | 911 | 11278 | 4830 | 1.124 | 583 |
