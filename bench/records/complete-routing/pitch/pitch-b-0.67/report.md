# Benchmark pitch-b-0.67

pcbench-b-fail, 11 boards, 20,000,000 work units per board, route options `--no-gpu --no-rescue --pitch-scale 0.67`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 9.1% | 0.0% |
| Mean completion | 88.6% | | |
| Route CPU time, all boards | 2,566 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 1.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 11 | 0.67 | 1 | 79 | 952 | 2797 | 1.106 | 89 |
| TraceMaker, boards with a reference | 11 | 0.67 | 1 | 79 | 952 | 2797 | 1.106 | 89 |
| Reference routing | 11 | 1.27 | 11 | 152 | 2288 | 219 | 1.149 | 94 |
