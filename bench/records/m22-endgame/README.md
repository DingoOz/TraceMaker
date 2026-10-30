# M22 items 1, 2 and 4: end-game retry, pin rescue, plane fallback (D155, doc 05 §63)

2026-10-29, branch `last-mile` on main `db5950f`. Machine shared by these runs only (three arms at a time, 10 boards each, `--threads 4`);
the budget is work units, so the load changes wall time, not the result.

## Ten failing boards, 8 variants, 20 M work

`lm-arms*.sh` holds the commands (`bench/run.py --set-name lm10 --boards ... --work W --jobs 10 --threads 4`). `runs/lm-*/` holds each
run's `summary.json`, `boards.jsonl` and `report.md`.

| Board | base | rescue in loop (v1) | base 22 M | end-game (v1) | end-game + rescue (v1) | end-game | end-game + rescue |
|---|--:|--:|--:|--:|--:|--:|--:|
| Aleste-520EX_aleste | 1351 | 1270 | 1362 | 1351 | 1351 | 1351 | 1351 |
| CoreOne-xCORE200-Original | 787 | 785 | 787 | 787 | 787 | 787 | 787 |
| EtherCAT_shield_v1 | 245 | 246 | 245 | 245 | 245 | 246 | 245 |
| LeeChee_1800 | 1131 | 1135 | 1135 | 1132 | 1132 | 1132 | 1132 |
| MonApollo_analog-board | 1126 | 1112 | 1129 | 1128 | 1130 | 1128 | 1130 |
| TCKB_Kicad_TCKB | 575 | 575 | 575 | 575 | 575 | 575 | 575 |
| kitspace_d20_tri_r0.3 | 410 | 408 | 410 | 410 | 410 | 410 | 410 |
| kitspace_teensy-fx | 185 | 186 | 185 | 185 | 185 | 185 | 186 |
| mini_mass_prog_bench_prog_rig | 986 | 985 | 986 | 986 | 986 | 986 | 986 |
| poncho_fpga_fpga-dongle | 379 | 347 | 379 | 379 | 379 | 379 | 383 |
| **total** | **7175** | **7049** | **7193** | **7178** | **7180** | **7179** | **7185** |

Runs: `lm-base`, `lm-rescue` (`--rescue 2,4,8`, then the default: rescue in the main loop, every victim re-routed), `lm-base22`
(`--work 22000000`), `lm-eg10` / `lm-eg10r` (`--endgame 0.1`, without / with `--rescue 2,4,8`; the retry ran only when the loop ended in
its best state, which after negotiation it usually did not), `lm-eg10b` / `lm-eg10rb` (the final code: back to the best state first, old
copper kept where it passes the exact check).

## One variant, 10 M work

`one-variant/`: the stats lines of `tracemaker route --variants 1 --work 10000000 --no-kb` (end-game arms: final code).

| Board | base | `--endgame 0.1` | `--endgame 0.1 --rescue 2,4,8` | `--work 11000000` |
|---|--:|--:|--:|--:|
| kitspace_teensy-fx | 167 | 167 (7 tried, 0 routed) | 167 (2 tried; 2 rescues, 0 routed) | 170 |
| poncho_fpga_fpga-dongle | 281 | 285 (11 tried, 4 routed) | 284 (6 tried, 3 routed; 5 rescues, 2 routed) | 282 |

## Options off

`bench/run.py --set quick --work 1000000 --threads 8` with this branch and with main `db5950f`: the 30 routed `.kicad_pcb` files are
byte-identical (`lm-quick-main`, `lm-quick-off` in `bench/results`).

## Conclusion

At the same budget, plain routing beats every arm (+18 against at best +10). The options stay off. The study's 139 open connections
that "route alone on the final board" came from 120 s runs that ended before the first pass did; at a fixed work budget the main loop
routes them anyway.
