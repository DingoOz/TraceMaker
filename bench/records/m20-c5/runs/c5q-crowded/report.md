# Benchmark c5q-crowded

quick, 30 boards, 1,000,000 work units per board, route options `--crowded-source`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 90.0% | 100.0% | 83.3% |
| Mean completion | 99.4% | | |
| Route CPU time, all boards | 576 s | | |

Both clean: 27; only TraceMaker: 0; only Freerouting: 3.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.79 | 0 | 34 | 305 | 540 | 1.059 | 42 |
| TraceMaker, boards with a reference | 30 | 0.79 | 0 | 34 | 305 | 540 | 1.059 | 42 |
| Reference routing | 30 | 1.23 | 75 | 209 | 1673 | 599 | 1.103 | 67 |
