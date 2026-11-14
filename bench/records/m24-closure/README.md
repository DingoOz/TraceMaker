# Placement closure (doc 04 §11, D190), 2026-11-13

Engine: branch `features-2026-11-13`. Board set: `closure-eval.txt`, 14 PCBench boards the router leaves incomplete with
the human placement (near misses of `pcbench-bd-fail` and the hard boards of doc 04 §8.3). Route: `bench/run.py --work W`
(`tracemaker route --work W --no-kb`, all variants). KiCad 10 DRC judges every output. "Complete" means KiCad counts
nothing unconnected. "Clean" means complete and no DRC error added relative to the human board; LimeSDR's DRC is capped
at 199 errors per type, so it can be complete but never clean.

| Run | What |
|---|---|
| `closure-human10m` | human placement, route at 10 M |
| `closure-human40m` | human placement, route at 40 M (control: 4× the router effort instead of placement) |
| `closure-place10m` | `--mode closure --closure-judge portfolio --route-check 10000000 --final-work 0`, then route at 10 M |
| `closure-route10m` | `--mode closure --route-check 10000000` (the product route judges each trial), then route at 10 M |
| `closure-route10m-then40m` | the `closure-route10m` placements routed at 40 M |

Placement: `--place-timeout 3000`, so `--loop-time 1800`; 7 boards in parallel; seed 1. `table.md` is
`bench/closure_table.py` over the five runs, and `table.json` holds the same data. `placed/` has the placer's reports
(every trial, each arm's trials and wins, the stop reason); `*.portfolio.placed.json` are the portfolio-judge runs.
`learned-prior.json` pools the arms of the route-judge run and is usable as `--closure-learn`.

## Results

| Run | Open | Complete | Clean | Worse than human at the same route |
|---|--:|--:|--:|--:|
| human, 10 M | 58 | 0 | 0 | |
| human, 40 M (control) | 40 | 3 | 3 | |
| closure, portfolio judge, 10 M | 47 | 0 | 0 | 2 boards (1Bitsy 1→3, 58r 6→7); 2 with placement DRC errors (see below) |
| **closure, route judge, 10 M** | **32** | **4** | **3** | 0 |
| **closure, route judge, then 40 M** | **30** | **5** | **4** | 1 (ardfpga 4→5 against human at 40 M) |

- **Boards made complete:**
  - 1Bitsy: C15 +0.5 mm, one trial.
  - Touch_Switch_2ch: crystal X3 −1 mm, one trial.
  - 58r-mxalps: R3 rotated, a window re-place of 15 parts, then PROGRAM +0.5 mm; 11 trials.
  - LimeSDR: C78 and C38 by 0.5 mm.
  - serial_gw: only with the 40 M route; closure has no legal move there.
- **Large reductions:**
  - CurrentSensor 6 → 1: D2 +4 mm, a window, then the room arm pushing C68 and C69.
  - teensy-fx 5 → 1: RV8 rotated, D5 and D4 swapped.
- **No change:** memsarray, serial_gw and phone_amp ran out of legal candidates ("no candidates"). The parts next to
  the failures overlap neighbours in the placer's model (21–43 conflicts in these hand placements), and a moved part
  must have none. robomezzi used its 30 minutes in 13 trials without a win.
- **Arms** (route-judge run, trials/wins): shift 51/7, rotate 28/3, window 37/3, room 6/1, swap 19/1, far 41/1,
  replace 32/0, flip 0/0 (not enabled). Small moves earned most of the wins. Whole-board re-placement never won on
  these boards, and most of its pulls were illegal placements, not routed.
- **Time:** closure takes 1,461 s per board at the median and 2,061 s at most (the 1,800 s loop limit plus the
  current trial). A 10 M route takes 114 s at the median and a 40 M route 213 s, so closure costs 10–15 routes per
  board.
- **The portfolio judge misleads.** Placements it preferred at 10 M were worse under the product route on 1Bitsy and
  58r, and reached no complete board. Its first version also tolerated a moved part's existing overlaps; KiCad then
  reported new errors on 1Bitsy. Strict legality was restored and these runs were repeated. The window arm also moved
  every part (CurrentSensor: 200 moved and a courtyard overlap) until it was restricted to its window. The phone_amp
  "+4 malformed courtyard" is a bench counting artefact: KiCad reports 82 on both the human and the placed board.
