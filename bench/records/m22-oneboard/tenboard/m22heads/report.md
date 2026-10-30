# Benchmark lm-m22heads

lm10, 10 boards, 20,000,000 work units per board, route options `--fanout-heads 5`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 0.0% | 0.0% |
| Mean completion | 86.5% | | |
| Route CPU time, all boards | 15,862 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 10 | 0.85 | 10 | 114 | 1803 | 5857 | 1.095 | 301 |
| TraceMaker, boards with a reference | 10 | 0.85 | 10 | 114 | 1803 | 5857 | 1.095 | 301 |
| Reference routing | 10 | 1.54 | 36 | 440 | 5104 | 4073 | 1.138 | 273 |
