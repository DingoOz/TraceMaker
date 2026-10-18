# Benchmark rel090w-tierB

PCBench tier B, 40 boards, 120 s per board.

| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|
| Clean pass (KiCad DRC) | 72.5% | 50.0% | 12.5% |
| Mean completion | 97.3% | | |
| Route CPU time, all boards | 14,809 s | | |

Both clean: 18; only TraceMaker: 11; only Freerouting: 2.
Boards with added DRC errors: 0 ({}).
