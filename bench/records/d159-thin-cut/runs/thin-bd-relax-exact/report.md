# Benchmark thin-bd-relax-exact

pcbench-bd-fail, 33 boards, 100,000,000 work units per board, route options `--no-gpu --relax --round-work 20000000 --rounds 5`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 15.2% | 12.1% | 0.0% |
| Mean completion | 91.5% | | |
| Route CPU time, all boards | 82,701 s | | |

Both clean: 0; only TraceMaker: 5; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 33 | 0.94 | 14 | 135 | 3780 | 13341 | 1.131 | 619 |
| TraceMaker, boards with a reference | 33 | 0.94 | 14 | 135 | 3780 | 13341 | 1.131 | 619 |
| Reference routing | 33 | 1.35 | 70 | 911 | 11278 | 4830 | 1.124 | 583 |
