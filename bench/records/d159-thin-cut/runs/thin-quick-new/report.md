# Benchmark thin-quick-new

quick, 30 boards, 1,000,000 work units per board, route options `--variants 1`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 36.7% | 100.0% | 83.3% |
| Mean completion | 89.8% | | |
| Route CPU time, all boards | 70 s | | |

Both clean: 11; only TraceMaker: 0; only Freerouting: 19.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.63 | 0 | 30 | 268 | 417 | 1.032 | 25 |
| TraceMaker, boards with a reference | 30 | 0.63 | 0 | 30 | 268 | 417 | 1.032 | 25 |
| Reference routing | 30 | 1.23 | 75 | 209 | 1673 | 599 | 1.103 | 67 |
