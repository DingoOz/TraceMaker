# Complete-routing records (doc 20)

Results behind [doc 20](../../../docs/20-complete-routing.md) steps 1-5, doc 05 §69-§71 and D161-D163. Branch `complete-routing` (on main
`d927bfc`), 2026-10-30. Machine: 56 cores, shared. No GPU (`--no-gpu`). The 33 boards are `bench/sets/pcbench-bd-fail.txt`, run with
`bench/run.py --set pcbench-bd-fail --work 100000000 --jobs N --threads 8 --time 3600` and KiCad-judged. `compare.py RUN...` gives the
per-board tables (the first run is the reference).

## Step 1: a full `--relax-late` run (`runs/late-bd`, `runs/late-vs-rescue.md`)

`--route-args "--no-gpu --relax --relax-late --no-rescue"`: 16,079 routed against the rescue's 16,208 (−129). 4 boards better, 15 worse,
3 clean against 5. Dropped by its stop rule.

## Step 2: the warm rescue (`runs/rescue-warm-bd`, `runs/warm-vs-fresh.md`, `runs/best-of-both.md`)

`--rescue-warm` (the experiment flag of the time, now `--rescue-mode warm`) against the fresh rescue (`bench/records/d160-rescue`,
`rescue-bd`):

| Rescue | Routed (of 16,935) | Better / worse vs fresh | Clean | CPU s |
|---|--:|--:|--:|--:|
| fresh | 16,208 | | 5 | 110,958 |
| warm | 16,259 (+51) | 14 / 5 | 4 | 86,304 |
| best of both per board | 16,280 (+72) | | 6 | about 133,000 |

Both run by default now, warm first (D163).

## Step 3: the realizer (`realizer/`, `runs/real-bd-relax`, `runs/real-srj6-relax`, `runs/realizer-*.md`)

- `realizer/`: the Python prototypes on poncho's dumped last hand-over (`TM_LEGALIZE_DUMP`). SLP (HiGHS) and SQP leave 7 pairs of 71;
  the penalty method and the augmented Lagrangian leave 92.
- `runs/real-bd-relax`: `--relax --round-work 20000000 --rounds 5 --no-rescue` with `TM_LEGALIZE_REALIZE=20`, against D159's
  `thin-bd-relax-final9`: 15,940 against 15,925 (+15, 5 better, 0 worse), 5 clean either way, no DRC error added.
- `runs/real-srj6-relax`: srj20's six stuck boards, `--relax --variants 8 --round-work 20000000 --rounds 5`, against
  `thin-srj6-relax`: 411 against 404 (+7, 4 better, 0 worse), none complete.

- `runs/real2-srj6-relax`: the same with the realizer judged by the routes the exact check keeps (D162 as adopted): 411, board for
  board the same as `real-srj6-relax`.

On by default now (D162).

## Step 4: why open (`escape/`)

See `escape/README.md`: no false alarm on 41,810 pins of 405 clean boards. On the 33 boards none of the 1,341 open pins is walled
in by the placement (D161).

## A finer lattice (`pitch/`)

See `pitch/README.md`: 0.67 and 0.5 of the automatic pitch route 11 and 10 fewer of 2,262 connections on the 11 failing tier B boards,
at 34 % and 117 % more CPU. Not adopted (doc 20 §2.7).

## The defaults together: quick tier (`quick/`)

See `quick/README.md`.
