# Benchmark c5m-crowded

mid, 11 boards, 20,000,000 work units per board, route options `--no-gpu --crowded-source`.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 0.0% | 9.1% | 0.0% |
| Mean completion | 80.0% | | |
| Route CPU time, all boards | 7,426 s | | |

Both clean: 0; only TraceMaker: 0; only Freerouting: 1.
Boards with added DRC errors: 0 ({}).

Style of the routed copper (doc 05 §39, informational):

| Routing | Boards | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 11 | 0.77 | 2 | 79 | 2132 | 5894 | 1.095 | 328 |
| TraceMaker, boards with a reference | 11 | 0.77 | 2 | 79 | 2132 | 5894 | 1.095 | 328 |
| Reference routing | 11 | 1.97 | 89 | 1019 | 14931 | 12507 | 1.132 | 380 |
