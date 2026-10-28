# Benchmark a5x-nocrowd-base-s1

planes, 8 boards, 3,000,000 work units per board, zones refilled before judging, route options `--soft-zones --seed 1 --critical-auto --no-crowded-portfolio`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 25.0% | 0.0% | 0.0% |
| Mean completion | 91.2% | | |
| Route CPU time, all boards | 796 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 0.
Boards with added DRC errors: 2 ({'solder_mask_bridge': 1, 'starved_thermal': 1}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 8 | 0.88 | 0 | 11 | 143 | 308 | 1.088 | 24 |
