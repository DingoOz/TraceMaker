# Benchmark thin-srj6-relax-sub

srj20-stuck6, 6 boards, 100,000,000 work units per board, route options `--no-gpu --relax --variants 8 --round-work 20000000 --rounds 5 --legalize-subdivide 0.1`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 0.0% | 0.0% |
| Mean completion | 89.2% | | |
| Route CPU time, all boards | 14,778 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 6 | 4.74 | 1 | 190 | 2031 | 879 | 1.349 | 91 |
