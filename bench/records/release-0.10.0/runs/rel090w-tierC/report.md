# Benchmark rel090w-tierC

PCBench tier C, 30 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 46.7% | 46.7% | 16.7% |
| Mean completion | 97.4% | | |
| Route CPU time, all boards | 14,994 s | | |

Both clean: 10; only TraceMaker: 4; only Freerouting: 4.
Boards with added DRC errors: 0 ({}).
