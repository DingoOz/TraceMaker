# M22 developed on one board: via shove, fan-out hold, negotiation bounds (D156, doc 05 §64)

2026-10-29/30, branch `last-mile` on main `db5950f`. The user's method: develop each M22 change on one board that no earlier run
completed, test only that board, and run the other failing boards only once M22 is developed on it. The board chosen from existing
data was `mini_mass_prog_bench_prog_rig` (995 connections, 4 layers, 600 x 110 mm; 986 routed at 20 M work in every earlier arm).

## One board: mini_mass, variant 6, 20 M work

The best variant of every eight-variant run is variant 6 ("fast bends, cheap crossings, escape plan, crowded source"), and only the
escape-plan variants (3 and 6) see the escape options, so the one-board runs ran it alone (`TM_PICK_VARIANTS=6`, a debugging switch;
70 s instead of 2.5 min). It reproduces the eight-variant result exactly (986, 4,649 tracks, 22,787,668 expansions). `runs/` holds
each run's log (search traces removed) and `route.json`.

| Arm | Routed | Note |
|---|--:|---|
| base (`v6base`) | 986 | open: U2.3, .16, .18, .25, .28, .39 (boxed in), .31, .32, .36 (INT nets, about 450 mm) |
| `--fanout-hold`, corridors 2 / 3 / 4 / 5 / 7 mm | 979 / 987 / 984 / 985 / 985 | the failures move between U2 pins; noise of ±2 |
| `--fanout-hold`, two-pin nets only, 3 / 5 mm | 986 / 983 | |
| `--escape-stubs --escape-length 5` | 986 | |
| `--work` 40 M / 60 M | 986 / 986 | the first pass is the same; negotiation spends the rest on 25-50 searches that fail at the 3 M cap |
| `--first-nets` the three INT nets / plus I2C_SCL | 661 / 10 | their whole-board searches take the budget |
| `--global` / `--global --global-confine` | 975 / 971 | |
| `--long-weight` 1.5 / 2 / 3 | 986 / 986 / 986 | INT #2 is found in 455 k expansions (3 M cap missed before), but negotiation is never reached at 20 M |
| `--neg-strict-cap 200000` (and with `--long-weight 2`), 20 M / 40 M / 80 M / 150 M | 986 throughout | every negotiated search finds a path; pass 2 never ends |
| `--pass-bound` with the two above (and `--soft-growth` 0.5 / 2), 40 M / 80 M | 986 throughout | passes end; routed by pass: 986, 970, 966, 961, 958, 971, 969, 974 |
| `--via-shove` (first version: every position searched) | 987 | I2C_SCL (U2.18) routes after an I2C_SDA via moves 0.45 mm; 1 of 165 attempts |
| `--via-shove` (final: lift first, then positions) | 987 | 54 searches |
| `--via-shove` with the negotiation options, 20 M / 40 M; `--via-shove --fanout-hold` | 987 / 987; 987 | |
| `--window-escalate`, `--via-retry` | 986, 986 | this board has no misses they act on |
| `--pin-via-pen 3` / `8` | 987 / 982 | |
| eight variants, `--via-shove` / with `--escape-all` | 987 / 987 | with `--escape-all` variants 1, 2, 5, 7 rise 976→987, 964→974, 976→984, 974→984 |

Search traces (`TM_DEBUG_CONN`, `runs/t2.searches.txt`): the stuck U2 pins are proved boxed in within 100-200 expansions in the first
pass. In negotiation each first tries strict searches that fail (LED1 2 x 6 M, INT #1 2 x 6 M, I2C_SCL 3.3 M: 25 M of 30 M), then a
negotiated search that finds a path in 18 k-1.4 M expansions. `runs/vc2.shove.txt`: with the nearest foreign vias lifted, one at a
time or in pairs, the other stuck pins stay boxed in: their blockers are the neighbours' tracks.

## Ten failing boards, 8 variants, 20 M work

`m22-all.sh` (as last run; the first run's arms were `--via-shove --pin-via-pen 3 --escape-all --window-escalate --via-retry
--pass-bound --neg-strict-cap 200000 --long-weight 2 --plane-informed` and `--via-shove --pin-via-pen 3 --escape-all`). `tenboard/<arm>/`
holds `summary.json`, `boards.jsonl`, `report.md` and every board's `route.json`. Base and base 22 M are the D155 runs
(`bench/records/m22-endgame`); with the options off the code is byte-identical to them (quick tier below).

| Board | base | base 22 M | all (unbounded shove) | core (unbounded shove) | shove | shove + escape-all |
|---|--:|--:|--:|--:|--:|--:|
| Aleste-520EX_aleste | 1351 | 1362 | 1097 | 1351 | 1351 | 1351 |
| CoreOne-xCORE200-Original | 787 | 787 | 792 | 792 | 788 | 788 |
| EtherCAT_shield_v1 | 245 | 245 | 245 | 245 | 247 | 248 |
| LeeChee_1800 | 1131 | 1135 | 1124 | 1136 | 1131 | 1131 |
| MonApollo_analog-board | 1126 | 1129 | 1113 | 1132 | 1119 | 1134 |
| TCKB_Kicad_TCKB | 575 | 575 | 582 | 582 | 575 | 582 |
| kitspace_d20_tri_r0.3 | 410 | 410 | 289 | 324 | 410 | 410 |
| kitspace_teensy-fx | 185 | 185 | 187 | 187 | 188 | 188 |
| mini_mass_prog_bench_prog_rig | 986 | 986 | 987 | 987 | 987 | 987 |
| poncho_fpga_fpga-dongle | 379 | 379 | 373 | 376 | 380 | 378 |
| **total** | **7175** | **7193** | **6789** | **7112** | **7176** | **7197** |

No arm added KiCad DRC errors on any board. The shove arms ran with the final shove (5 % of the work budget at most); "all" and "core"
with the first, unbounded one.

Attribution (`attribution/`, one option at a time, 20 M work, 8 variants):

| Board | Option | Routed | vs base |
|---|---|--:|--:|
| Aleste | `--window-escalate` (15 escalations) | 1097 | -254 |
| Aleste | `--via-retry`, `--pass-bound`, `--neg-strict-cap 200000`, `--long-weight 2`, `--plane-informed` | 1351 each | 0 |
| kitspace_d20 | `--via-shove`, unbounded (10 shoves, 531 searches) | 379 | -31 |
| kitspace_d20 | `--via-shove`, at most 10 % / 5 % of the budget | 408 / 410 | -2 / 0 |
| kitspace_d20 | `--pin-via-pen 3` | 401 | -9 |
| kitspace_d20 | `--escape-all` | 410 | 0 |
| poncho | `--via-shove` unbounded / 10 % | 380 / 380 | +1 |
| poncho | `--pin-via-pen 3` | 378 | -1 |
| poncho | `--escape-all` | 374 | -5 |

## Quick tier with the options off

`quick-off/`: 30 boards, 1 M work, every output board byte-identical to `lm-quick-off` (the D155 run, itself identical to main `db5950f`).
