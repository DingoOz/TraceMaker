# Benchmark closure-human10m

closure-eval, 14 boards, 10,000,000 work units per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 14.3% | 0.0% |
| Mean completion | 95.8% | | |
| Route CPU time, all boards | 9,093 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 2.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 14 | 0.65 | 6 | 76 | 1628 | 3294 | 1.108 | 159 |
| TraceMaker, boards with a reference | 14 | 0.65 | 6 | 76 | 1628 | 3294 | 1.108 | 159 |
| Reference routing | 14 | 1.02 | 12 | 231 | 2828 | 458 | 1.117 | 152 |
