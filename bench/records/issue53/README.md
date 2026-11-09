# Issue #53 records: a clearance rule below the class (D170)

Boards:

- The 10-port PoE switch WholeBoard designed (240 x 140 mm, four layers, 1,189 connections, 81 differential pairs by name), placed
  revision `rev7/poe_gbe_switch_10p-2-placed.kicad_pcb` of WholeBoard project
  `20261027-015953-38a2-i-would-like-an-8-port-ethernet-switch-2-10port`. Not committed (private); the same board as `../d139/`.
- StickHub, the KiCad demo (USB hub, 8 USB pairs, class and board clearance 0.15 mm), stripped by `bench/prepare_demos.py`
  (`build/demos/StickHub/unrouted.kicad_pcb`). Public repro.

Rules files beside the board copies: `nonzone-0.09.kicad_dru` (WholeBoard's JLCPCB clearance rule, the issue's), `all-0.09.kicad_dru`
(unconditional 0.09 mm), `all-0.15.kicad_dru` (unconditional 0.15 mm, the class value), or none.

"main" is origin/main 057ee82; "D170" is this branch. All runs `--diff-pairs --no-kb --no-rescue --no-why-open`, 2026-11-08, the shared
machine with other jobs running.

## Where the time goes

`poe-1v-300s-base-*-pairs.log` (main, one variant, `--time 300`, `TM_DEBUG_TIMING=1 TM_DEBUG_PAIRS=1`, each line prefixed with the Unix
time): the router was still in the coupled pair phase at 300 s with and without the rule (55 and 60 pairs coupled). Pairs that end at
the 250 k-state cap without a legal end take 10-36 s each. `gprof-base-rule-flat.txt` (main, CPU build with `-pg`, rule, 600 k work):
the condition's string work (`str_eq`, `wildcard_match`, string copies, `kind_name`) and `custom_min` sit among the top entries beside
the exact checks.

## One variant, fixed work (deterministic)

`tracemaker route b.kicad_pcb -o out.kicad_pcb --diff-pairs --no-kb --variants 1 --work W --time 1500 --no-rescue --no-why-open`

| Board | Rules | W | main | D170 | Output main = D170 |
|---|---|--:|--:|--:|---|
| PoE switch | non-zone 0.09 | 2 M | 189.1 s, 30 routed | 85.9 s, 30 routed | yes |
| PoE switch | all 0.09 | 2 M | 122.3 s, 30 routed | 84.1 s, 30 routed | yes |
| PoE switch | none | 2 M | 134.6 s, 22 routed | | |
| PoE switch | all 0.15 | 2 M | 137.4 s, 22 routed (same file as none) | | |
| StickHub, `--soft-zones` | non-zone 0.09 | 5 M | 31.8 s CPU, 128/128 | 21.0 s CPU, 128/128 | yes |
| StickHub, `--soft-zones` | none | 5 M | 31.0 s CPU, 122/128 | 30.9 s CPU, 122/128 | yes |

The main runs of each pair ran at the same time; the PoE main and D170 rows ran at different times (machine load differs), so compare
the non-zone rule with the unconditional one within a column: 55 % slower on main, 2 % with D170.
Logs: `poe-1v-2M-{base,new}-*.log`, `stickhub-*-5M-{zbase,znew}.log`.

`--pair-share` at fixed work (D170):

| Board | W | Share | Routed | Coupled pairs | Wall | Log |
|---|--:|--:|--:|--:|--:|---|
| PoE switch, non-zone 0.09 | 5 M | 0 | 88 | 44 | 211 s | `poe-1v-5M-new-share0.log` |
| PoE switch, non-zone 0.09 | 5 M | 0.25 | 126 | 8 | 65 s | `poe-1v-5M-new-share25.log` |
| StickHub, non-zone 0.09 | 5 M | 0.25 | 128/128 | 7 (6 without) | 29.1 s | `stickhub-rule-5M-z25.log` |

## Eight variants, 300 s (as `../d139/`)

`tracemaker route b.kicad_pcb -o out.kicad_pcb --diff-pairs --no-kb --time 300 --no-rescue --no-why-open`; the four runs at the same
time (32 threads on the shared machine).

| Engine | Rules | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | Best | Coupled (best) | Work | Log |
|---|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|---|
| main | non-zone 0.09 | 96 | 96 | 160 | 96 | 96 | 1097 | 96 | 54 | 1097 | 75 | 44.7 M | `poe-8v-300s-base-rule.log` |
| D170 | non-zone 0.09 | 126 | 140 | 660 | 140 | 140 | 1105 | 126 | 106 | 1105 | 75 | 89.7 M | `poe-8v-300s-new-rule.log` |
| D170 `--pair-share 0.25` | non-zone 0.09 | 1027 | 1104 | 1029 | 1073 | 1034 | 1105 | 1111 | 1088 | 1111 | 19 | 404.6 M | `poe-8v-300s-new-share25-rule.log` |
| D170 | none | 106 | 106 | 148 | 106 | 106 | 1090 | 106 | 86 | 1090 | 70 | 80.8 M | `poe-8v-300s-new-norule.log` |

With the kind tables the rule no longer costs more than no rule (the variants that stall now stall in the pair phase with or without
it). The share makes every variant route the board, but the kept result gains 6 connections and loses 56 coupled pairs, so it stays
off by default.

## Quick tier

Not run: none of its 30 boards has a custom rule (no `.kicad_dru`), so the kind tables are never built there, and `--pair-share` is off by
default; the change cannot reach them.
