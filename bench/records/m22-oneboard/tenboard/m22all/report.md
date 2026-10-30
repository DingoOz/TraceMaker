# Benchmark lm-m22all

lm10, 10 boards, 20,000,000 work units per board, route options `--via-shove --pin-via-pen 3 --escape-all --window-escalate --via-retry --pass-bound --neg-strict-cap 200000 --long-weight 2 --plane-informed`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 0.0% | 0.0% |
| Mean completion | 79.3% | | |
| Route CPU time, all boards | 15,637 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 10 | 0.81 | 7 | 79 | 1523 | 5035 | 1.090 | 282 |
| TraceMaker, boards with a reference | 10 | 0.81 | 7 | 79 | 1523 | 5035 | 1.090 | 282 |
| Reference routing | 10 | 1.54 | 36 | 440 | 5104 | 4073 | 1.138 | 273 |
