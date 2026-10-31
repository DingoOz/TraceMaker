# Quick tier with the new defaults (D161, D162, D163)

`bench/run.py --set quick --work 1000000 --route-args "--variants 1" --jobs 4 --threads 8`, against D160's `rescue-quick` (main at D160:
the fresh rescue, no realizer). The rescue runs relax on the quick boards left open, so the realizer and the two-pass rescue reach them.

| Run | Routed (of 2,018) | Better / worse | Clean | Gates |
|---|--:|--:|--:|---|
| `rescue-quick` (D160) | 1,740 | | 12 | |
| `cr-quick`: realizer judged by its pairs, both rescue passes | 1,828 (+88) | 17 / 1 | 16 | **fail**: uC3Moy 40 → 39 |
| `cr-quick2`: realizer judged by the routes kept (D162), both passes | 1,829 (+89) | 17 / 0 | 16 | pass |

- `cr-quick`'s loss: on uC3Moy the fresh pass's realizer took the last hand-over from 9 pairs to 5, but the router's conflict-free
  subset then kept 39 routes, against 40 after the projection alone. The router now legalises both ways and keeps the result that
  keeps more routes.
- `quick-compare.md` is `bench/compare_runs.py rescue-quick cr-quick2` (12 of 30 boards byte-identical, gates pass). `per-board.md` has
  all three runs.
- No board added a DRC error. Router seconds 70.9 → 66.7.
