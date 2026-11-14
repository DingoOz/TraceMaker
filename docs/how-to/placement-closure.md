---
status: current
code: [src/place/closure.cpp, src/place/closure.hpp, src/place/place_main.cpp]
---

# Move parts until a board routes

When `tracemaker route` leaves connections open, the placement can be the cause. A part may sit where a track or via
cannot pass, or turned so that its pins face the wrong way. `tracemaker-place --mode closure` moves parts until the
same route completes. It tries small changes first and bigger ones when those stop helping, and it learns which kinds
of change pay off on the board (design doc 04 §11, decision [D190](../decisions/D190.md)).

## Run it

```
tracemaker-place board.kicad_pcb -o board.placed.kicad_pcb --mode closure --route-check 10000000 --loop-time 1800
tracemaker route board.placed.kicad_pcb -o board.routed.kicad_pcb --work 10000000 --no-kb
```

- `--route-check` is the route's work budget. Use the budget you will route with: each trial is judged by
  `tracemaker route --work <budget> --no-kb` itself, so a kept change routes better under exactly that route. Add
  options with `--closure-route-args "--variants 4"`.
- `--loop-time` stops the search after that many seconds and keeps the best placement so far. `--closure-routes`
  caps the number of trials (default 40).
- If the board already routes completely, the output is the input, unchanged.
- Locked parts never move, and neither do edge connectors unless you pass `--move-connectors`. A moved part never
  overlaps anything at its new place. Add `--flip` to let surface-mount parts change side.

## Read what it did

The log lists every trial: the kind of change, the part, and the open connections before and after. Kept changes are
marked `accepted`. `--json report.json` writes the same list under `closure.trials`, the trials and wins of each kind
under `closure.arms`, and why the search stopped under `closure.stop` (`complete`, `route budget`, `time limit` or `no
candidates`).

## Let it learn across boards

```
tracemaker-place a.kicad_pcb -o a.placed.kicad_pcb --mode closure --closure-learn ~/.cache/tracemaker/closure.json
tracemaker-place b.kicad_pcb -o b.placed.kicad_pcb --mode closure --closure-learn ~/.cache/tracemaker/closure.json
```

The file holds the trials and wins of each kind of change. It is read before the run as the bandit's prior and
updated afterwards. `bench/records/m24-closure/learned-prior.json` is the table from the 14-board evaluation.

## When it cannot help

- **No change it may make.** The parts next to the open connections overlap their neighbours in the placer's model,
  so none of them can move. The model's courtyards are convex hulls and the through-hole margins are conservative,
  so hand placements that KiCad accepts often have such overlaps. The log then says `no candidates`.
- **The open pin is boxed in by its own package.** Dense packages whose pins cannot escape need a different
  footprint, more layers (`tracemaker route --try-layers 2,4`) or a finer via. Moving parts around them does not help.
