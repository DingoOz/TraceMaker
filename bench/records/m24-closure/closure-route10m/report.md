# Benchmark closure-route10m

closure-eval, 14 boards, 10,000,000 work units per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 21.4% | 14.3% | 0.0% |
| Mean completion | 96.6% | | |
| Route CPU time, all boards | 6,085 s | | |

Both clean: 1; only TraceMaker: 2; only Freerouting: 1.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 14 | 0.71 | 3 | 33 | 978 | 2522 | 1.112 | 143 |
| TraceMaker, boards with a reference | 14 | 0.71 | 3 | 33 | 978 | 2522 | 1.112 | 143 |
| Reference routing | 14 | 1.02 | 12 | 231 | 2828 | 458 | 1.117 | 152 |
