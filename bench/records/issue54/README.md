# Issue #54: obstacle caches keyed by class values, with a memory bound (D169)

Behind [D169](../../../docs/decisions/D169.md) and doc 05 §72. Branch `class-cache-bound` (on main `b863430`), 2026-11-03.

The issue's board (a 10-port PoE switch, 83 classes, one per impedance pair) is not public. Its shape is many classes with the same
values, so two KiCad demos (`bench/prepare_demos.py`) were routed with their own classes and with every net in a class of its own,
keeping the values of the class it had (`split_classes.py`). Same options for every run:

```
tracemaker route unrouted.kicad_pcb -o out.kicad_pcb --work 3000000 --variants 2 --threads 2 --no-gpu --no-kb --time 3600 --view
```

Peak memory and wall time from `/usr/bin/time -v`; the other columns from the route log.

| Board, classes | Engine | Peak memory | Wall time | Fixed-copper checks | Routed | Output |
|---|---|--:|--:|--:|--:|---|
| interf_u, 2 | main | 886 MB | 37.8 s | 3,140,539 | 120/164 | A |
| interf_u, 175 (one per net) | main | 1,185 MB | 48.8 s | 5,284,040 | 120/164 | A |
| interf_u, 2 | D169 | 892 MB | 37.1 s | 3,140,539 | 120/164 | A |
| interf_u, 175 | D169 | 885 MB | 37.0 s | 3,140,539 | 120/164 | A |
| interf_u, 175, `--class-cache-mb 20` | D169 | 841 MB | 40.4 s | 3,569,848 (17 evictions) | 120/164 | A |
| StickHub, 1 | main | 296 MB | 29.6 s | 681,412 | 24/128 | B |
| StickHub, 48 (one per net) | main | 305 MB | 28.8 s | 869,604 | 24/128 | B |
| StickHub, 1 | D169 | 297 MB | 30.0 s | 681,412 | 24/128 | B |
| StickHub, 48 | D169 | 297 MB | 29.3 s | 681,412 | 24/128 | B |

"Output" compares the written boards without uuids: every run of a board wrote the same copper. On main, 175 classes cost a third more
memory and time on interf_u (one cache per class); with D169 the split board makes the same single set of caches as the original, and
its work counts are identical. The memory the issue saw (181 GB) came from full arrays per class before D154 made them grow by chunks,
times 83 classes, times 8 variants.

The bounded run freed caches 17 times, took 3 s more, and here routed the same board. Its expansion count differs (3,033,691 against
3,032,059), since the cost-to-go fields use only the codes already known: a bounded run is deterministic but may route differently from
an unbounded one, as D169 says.

StickHub stops at 24/128 at this budget on both engines (the rescue's relax runs reach 19 and are not kept); it is here for the work
counts, not the completion.

## Quick tier

`bench/run.py --set quick --work 1000000 --jobs 2 --threads 4 --view` with main's engine and this branch's.

- Run one after the other (`issue54-base-six`, `issue54-six`; `quick/sequential.md`): routed boards byte-identical on **30/30**, clean
  29 and 29. Route CPU time 453 → 499 s (+10%) at the same work, with byte-identical boards moving from -32% to +64%: the machine was
  shared, and the interf_u and StickHub runs above show no slowdown.
- A first pair run at the same time from two worktrees (`issue54-base-quick`, `issue54-quick`; `quick/concurrent.md`) was
  byte-identical on 24/30; on the other six one side did not route, as both runs claimed the same viewer ports
  (`error: viewer server: cannot listen on 0.0.0.0:8800`).

## Files

- `split_classes.py`: puts every net of a project in a class of its own.
- `runs/<board>-<old|new>-<one|split>[-mb20]/` (old = main, new = D169; one = the project's classes, split = one per net): route log (`route.log`) and `/usr/bin/time -v` output with the run's stderr (`time.log`).
- `quick/`: `compare_runs.py` output for both quick-tier pairs, and the sequential pair's summaries.
