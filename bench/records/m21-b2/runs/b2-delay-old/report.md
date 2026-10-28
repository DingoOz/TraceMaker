# Benchmark b2-delay-old

b2-delay, 2 boards, 3,000,000 work units per board, zones refilled before judging, route options `--soft-zones`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 0.0% | 0.0% |
| Mean completion | 77.3% | | |
| Route CPU time, all boards | 182 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 2 ({'skew_out_of_range': 2, 'starved_thermal': 1}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 2 | 1.23 | 0 | 6 | 69 | 103 | 1.153 | 16 |
