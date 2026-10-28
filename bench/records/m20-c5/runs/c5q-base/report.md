# Benchmark c5q-base

quick, 30 boards, 1,000,000 work units per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 83.3% | 100.0% | 83.3% |
| Mean completion | 99.0% | | |
| Route CPU time, all boards | 446 s | | |

Both clean: 25; only TraceMaker: 0; only Freerouting: 5.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.74 | 0 | 35 | 364 | 761 | 1.056 | 35 |
| TraceMaker, boards with a reference | 30 | 0.74 | 0 | 35 | 364 | 761 | 1.056 | 35 |
| Reference routing | 30 | 1.23 | 75 | 209 | 1673 | 599 | 1.103 | 67 |
