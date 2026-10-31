# Is the lattice too coarse? (doc 20 §2.7)

Asked 2026-10-30: "is the granularity of the grid hurting completion?" Coordinates are int64 nanometres, so the number type is no limit;
the search lattice is ((width + clearance) / 6, 25-100 µm, doc 05 §25). `route --pitch-scale X` (hidden, experiment) multiplies the
automatic pitch on every board and variant.

`bench/run.py --board-list <the 11 tier B boards of pcbench-bd-fail> --work 20000000 --jobs 3 --threads 8 --time 3600
--route-args "--no-gpu --no-rescue --pitch-scale X"`:

| Pitch scale | Routed (of 2,262) | vs 1.0 | Better / worse | CPU s | Wall s |
|---|--:|--:|--:|--:|--:|
| 1.0 (automatic) | 2,051 | | | 1,918 | 313 |
| 0.67 | 2,040 | −11 | 2 / 5 | 2,566 | 468 |
| 0.5 | 2,041 | −10 | 2 / 5 | 4,158 | 753 |

- At the same work a finer lattice routes slightly less: a path takes more steps, so the budget explores less. It costs 34 % and
  117 % more CPU. No run hit the time limit.
- Gains on two dense boards (teensy-fx 185 → 189, E3 309 → 314), losses on five (poncho 379 → 366-368, ardfpga, EtherCAT, OtterPill).
- The automatic pitch stays. The room the lattice wastes is better recovered off-lattice (relax and the legaliser, doc 05 §41.3, §70),
  and the portfolio's cheap-vias variant already routes small boards at half pitch.

`compare.md`: per board.
