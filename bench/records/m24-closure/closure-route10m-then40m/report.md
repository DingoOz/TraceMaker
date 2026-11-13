# Benchmark closure-route10m-then40m

closure-eval, 14 boards, 40,000,000 work units per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 28.6% | 14.3% | 0.0% |
| Mean completion | 96.8% | | |
| Route CPU time, all boards | 21,149 s | | |

Both clean: 1; only TraceMaker: 3; only Freerouting: 1.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 14 | 0.63 | 4 | 69 | 1623 | 3779 | 1.111 | 109 |
| TraceMaker, boards with a reference | 14 | 0.63 | 4 | 69 | 1623 | 3779 | 1.111 | 109 |
| Reference routing | 14 | 1.02 | 12 | 231 | 2828 | 458 | 1.117 | 152 |
