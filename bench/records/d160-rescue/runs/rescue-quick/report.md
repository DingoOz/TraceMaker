# Benchmark rescue-quick

quick, 30 boards, 1,000,000 work units per board, route options `--variants 1`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 40.0% | 100.0% | 83.3% |
| Mean completion | 90.1% | | |
| Route CPU time, all boards | 91 s | | |

Both clean: 12; only TraceMaker: 0; only Freerouting: 18.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.62 | 0 | 29 | 268 | 688 | 1.031 | 24 |
| TraceMaker, boards with a reference | 30 | 0.62 | 0 | 29 | 268 | 688 | 1.031 | 24 |
| Reference routing | 30 | 1.23 | 75 | 209 | 1673 | 599 | 1.103 | 67 |
