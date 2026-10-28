# Benchmark b1-v5

b1-profiles, 5 boards, 3,000,000 work units per board, zones refilled before judging, route options `--soft-zones`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 0.0% | 0.0% |
| Mean completion | 77.0% | | |
| Route CPU time, all boards | 495 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 1 ({'diff_pair_gap_out_of_range': 1}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 5 | 0.78 | 0 | 5 | 281 | 254 | 1.111 | 27 |
