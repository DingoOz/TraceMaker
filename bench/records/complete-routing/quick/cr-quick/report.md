# Benchmark cr-quick

quick, 30 boards, 1,000,000 work units per board, route options `--variants 1`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 53.3% | 100.0% | 83.3% |
| Mean completion | 93.9% | | |
| Route CPU time, all boards | 92 s | | |

Both clean: 16; only TraceMaker: 0; only Freerouting: 14.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 30 | 0.67 | 0 | 29 | 275 | 628 | 1.038 | 29 |
| TraceMaker, boards with a reference | 30 | 0.67 | 0 | 29 | 275 | 628 | 1.038 | 29 |
| Reference routing | 30 | 1.23 | 75 | 209 | 1673 | 599 | 1.103 | 67 |
