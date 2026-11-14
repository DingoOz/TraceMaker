# M23 E3: octilinear doglegs (D187, doc 05 §80)

`run.sh`: tiers A and B (40 boards each, seed 1), `--work 3000000` (deterministic), 4 threads per board, binary of commit 3589211 with the
option as it was then (`--octilinear-min-leg` default 1). Three arms: no pass, `--octilinear` (shortest leg 1 track width), and
`--octilinear --octilinear-min-leg 0.5`. Off-angle segments counted on the output boards with `bench/octilinear_count.py` (the engine's
test: more than 0.1 degree from an axis and from a diagonal; short = shorter than its width); `e3-count-tier{A,B}.json` has them per board.

| tier | arm | routed | clean | DRC errors added | segments | off-angle | off-angle, mm | short | wall, sum s |
|---|---|---|---|---|---|---|---|---|---|
| A | none | 2,637 / 2,637 | 40 | 0 | 7,620 | 768 | 561 | 457 | 762 |
| A | min leg 1 | 2,637 / 2,637 | 40 | 0 | 7,878 | 510 (−34 %) | 331 | 457 | 753 |
| A | min leg 0.5 | 2,637 / 2,637 | 40 | 0 | 8,049 | 339 (−56 %) | 226 | 659 | 747 |
| B | none | 5,103 / 5,380 | 24 | 0 | 16,813 | 2,891 | 2,780 | 1,320 | 1,851 |
| B | min leg 1 | 5,103 / 5,380 | 24 | 0 | 17,388 | 2,316 (−20 %) | 2,281 | 1,321 | 1,846 |
| B | min leg 0.5 | 5,103 / 5,380 | 24 | 0 | 17,798 | 1,906 (−34 %) | 2,015 | 1,833 | 1,845 |

Over both tiers: 3,659 off-angle segments, 2,826 with min leg 1 (−23 %), 2,245 with min leg 0.5 (−39 %). The gate of doc 18 §20.2 (a third
fewer, no routed connection lost, no DRC error added) is met with min leg 0.5 only, which became the default. The price is legs between half a
width and a width long: 712 more segments shorter than their width over the 80 boards.

The judge's own style count (`style.off_angle_segments` in `boards.jsonl`) agrees: 768, 510 and 339 on tier A; 2,891, 2,316 and 1,906 on
tier B. Its `style.bends` is the same in all three arms (3,220 on tier A).
