# Benchmark vfix-bd-relax2

pcbench-bd-fail, 33 boards, 100,000,000 work units per board, route options `--no-gpu --relax --round-work 20000000 --rounds 5 --no-rescue --ramp-vias-fixed`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 9.1% | 12.1% | 0.0% |
| Mean completion | 91.6% | | |
| Route CPU time, all boards | 60,114 s | | |

Both clean: 0; only TraceMaker: 3; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 33 | 0.97 | 14 | 122 | 3226 | 12414 | 1.125 | 630 |
| TraceMaker, boards with a reference | 33 | 0.97 | 14 | 122 | 3226 | 12414 | 1.125 | 630 |
| Reference routing | 33 | 1.35 | 70 | 911 | 11278 | 4830 | 1.124 | 583 |
