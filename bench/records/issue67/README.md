# Issue #67: coupled pairs on boards with pads beside the pair (D175)

Behind [D175](../../../docs/decisions/D175.md) and doc 05 §77. Branch `issue67-coupled-pairs` (on main `057ee82`), 2026-11-08.
The issue and its design are by quanln (GitHub @quanln); their board is internal, so these runs use the public boards with
differential pairs: the 18 PCBench boards of doc 05 §15 and two KiCad demos with USB pairs (StickHub, 8 pairs; RoyalBlue54L-Feather, 1).

## How

`run_all.sh BINARY OUT JOBS NAME=ARGS ...` runs `bench/pair_eval.py` (now with `--mode NAME=ARGS`, `--tm` and `--time`) on
`boards.txt`: one variant, one thread, `--work 30000000 --time 3600 --no-kb --no-gpu`, so the work budget ends every run. The demos get
`--soft-zones` as well (their pads are walled in by zone fills otherwise; 25 of 128 routed). Coupled share, gap and skew come from
`tracemaker pairs`, DRC from KiCad 10.0.6 (errors added by routing, `bench/drc_judge.py`). `summarize.py` writes the tables
(`pcbench-tables.md`, `demos-tables.md`) from the JSON files here.

- `base-*.json`: main (`057ee82`), `--diff-pairs`.
- `new-*.json`: the first build of the branch. `leg4` / `leg6` = `--pair-leg-mm 4` / `6`, `prot` = `--protect-coupled`, `recp` =
  `--recouple-work 2000000`; `exits_v1` and `all_v1` (all four together) used a first version of `--pair-pad-exits` that mixed exit legs
  with the direct ones; it was replaced (exit legs only where no direct leg is legal) and is kept only as a record.
- `new2-*.json`: the branch as committed, before the default change: `base` (all options off; byte-identical to main on all 20
  boards), `exits` = `--pair-pad-exits`, `retry6` = `--pair-retry-leg-mm 6`, `retry6r` = the same plus `--recouple-work 2000000`.

## Results: 18 PCBench boards (60 pairs, 2,403 connections)

| Options | >= 80 % | >= 50 % | > 0 % | Median share | Mean share | Routed | KiCad errors added | Clean boards |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| main (`--diff-pairs`) | 19 | 33 | 47 | 60 % | 48.9 % | 2,248 | 0 | 9 |
| `--pair-leg-mm 4` | 15 | 29 | 45 | 47 % | 43.7 % | 2,248 | 0 | 9 |
| `--pair-leg-mm 6` | 11 | 33 | 46 | 58 % | 46.4 % | 2,248 | 0 | 9 |
| `--protect-coupled` | 16 | 30 | 44 | 51 % | 45.3 % | 2,245 | 0 | 7 |
| `--recouple-work 2000000` (now the default) | 20 | 34 | 48 | 65 % | 50.8 % | 2,248 | 0 | 9 |
| `--pair-pad-exits` | 15 | 30 | 46 | 50 % | 46.1 % | 2,249 | 0 | 9 |
| `--pair-retry-leg-mm 6` | 19 | 35 | 47 | 60 % | 49.9 % | 2,250 | 0 | 9 |
| `--pair-retry-leg-mm 6 --recouple-work 2000000` | 20 | 36 | 48 | 65 % | 51.4 % | 2,250 | 0 | 9 |

Per board and pair: `pcbench-tables.md`. What the numbers say:

- **A fixed longer reach loses coupling.** The pair search takes the first legal goal candidate it pops, and with a reach of 4-6 mm that
  is often a candidate far from the pads: the hub's fifth pair 92 → 79 / 74 %, edid-injector 88 → 85 / 80 %, USB-C-Screen-Adapter
  63 → 0 % and 87 → 0 % at 4 mm. A few gained (PmodHDMIIn's third pair 4 → 44 %), more lost. It helps quanln's board (CHG_D 0 → 90.4 % at 6 mm), where the pads
  of other nets leave no legal start within 2.5 mm, so it stays an option.
- **Protecting coupled halves costs connections**: 3 fewer routed in all (hub 113 → 111, OtterPillG 109 → 107, USB-C-Screen
  132 → 131, PmodHDMIIn 112 → 114), 2 fewer clean boards, the hub's run took 95 s instead of 12 s (the whole budget), and coupling fell on balance. Opt-in, as quanln found
  (3 more open signal connections on their board).
- **The re-coupling allowance only adds.** 18 of 20 boards are byte-identical to main; PmodHDMIIn's third pair 4 → 29 % and
  USB-C-Screen-Adapter's L2 pair 0 → 89 %, with the same routed count and no DRC error. The default is now 2,000,000.
- **Pad exits** mix: PmodHDMIIn routes 116 / 116 (from 112) and RaspberryPi-PoE's second pair 24 → 73 %, but USB-C-Screen-Adapter loses
  coupling on four pairs and two connections, EtherCAT two connections. Opt-in.
- **The wider retry** (`--pair-retry-leg-mm 6`: on "no coupled path", again with 6 mm legs, then with pad exits) coupled no public pair
  by itself (`pair_retried` 0 on all 20 boards); the work the failed retries spend shifts the rest of the run (PmodHDMIIn's pairs
  44 6 4 48 → 55 8 41 56 %, stack-light and CH330 one more connection, StickHub's first pair 7 → 0 %). Opt-in.

## Results: two KiCad demos (9 pairs, `--soft-zones`)

| Options | >= 50 % | > 0 % | Median share | Routed | KiCad errors added |
|---|--:|--:|--:|--:|--:|
| main | 2 | 7 | 31 % | 253 / 275 | 427 |
| `--pair-leg-mm 6` | 1 | 7 | 31 % | 255 / 275 | 427 |
| `--protect-coupled` | 1 | 5 | 32 % | 252 / 275 | 480 |
| `--recouple-work 2000000` | 2 | 7 | 31 % | 253 / 275 | 427 |
| `--pair-pad-exits` | 2 | 6 | 31 % | 252 / 275 | 441 |
| `--pair-retry-leg-mm 6` | 2 | 6 | 31 % | 253 / 275 | 426 |

The KiCad errors are StickHub's, clearance to zone fills that `--soft-zones` routes through and KiCad's DRC judges from the file's fills;
they are there on main too and are not a pair matter. The re-coupling allowance is byte-identical to main on both demos.

## Under a time limit

EtherCAT_shield, `--time 30`, one variant: the same four pairs with and without `--recouple-work` (80 15 78 22 %); no pair of it was
split, so the clean-up had nothing to re-couple. The time conversion is exercised, not measured on a board that needs it.

## Unit tests

`tests/test_diff_pair.cpp`: a pair whose pads sit behind a field of posts couples only with `--pair-leg-mm 6` (or the retry), a pair
whose pads are walled in on three sides couples only with `--pair-pad-exits` (or the retry), and the default reach is the old one.
