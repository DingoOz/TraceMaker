# Issue #42 records: copper against the outline at zero clearance (D120)

KiCad 10.0.6 (`kicad/kicad:10.0.6`, `kicad-cli pcb drc`) against `tracemaker drc` before and after the fix, 2026-10-16.

## What KiCad does, by shape

A 10 mm square outline (`gr_rect`, stroke 0.05 mm; four `gr_line`s give the same result), project `min_copper_edge_clearance` 0, one item whose copper edge lies
`offset` nanometres past the outline's centre line. Count of `copper_edge_clearance` violations in KiCad (the engine matches every cell after the fix; before it,
it flagged the touching cell of the track, via, pad_circle, pad_oval and pad_roundrect rows):

| item | in 1 nm | touching | +1 nm | +499 nm | +500 nm | +501 nm | +1000 nm |
|---|---:|---:|---:|---:|---:|---:|---:|
| track | 0 | 0 | 1 | 1 | 1 | 1 | 1 |
| via | 0 | 0 | 1 | 1 | 1 | 1 | 1 |
| arc | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| pad_rect | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| pad_trapezoid | 0 | 1 | 1 | 1 | 1 | 1 | 1 |
| pad_circle | 0 | 0 | 1 | 1 | 1 | 1 | 1 |
| pad_oval | 0 | 0 | 1 | 1 | 1 | 1 | 1 |
| pad_roundrect | 0 | 0 | 1 | 1 | 1 | 1 | 1 |

The rect and trapezoid pads and the arc are flagged by KiCad when touching, so they were right before.

Required clearance 0.1 mm, a straight track whose copper edge is 100 µm minus 0, 1, 499, 500, 501 and 1000 nm from the centre line: 0 | 0 | 0 | 0 | 1 | 1
(flagged from a gap below 99.5 µm, the clearance minus KiCad's 0.5 µm epsilon).

`kicad-10.0.6-edge-counts.json` is the frozen oracle that `tests/integration/edge_touch/run.py` compares with (71 cases; `--rejudge` measures it again).
Before the fix that test failed on 7 cases (`track_touch`, `track_touch_lines`, `via_touch`, `via_touch_lines`, `pad_circle_touch`, `pad_oval_touch`, `pad_roundrect_touch`:
TraceMaker 1, KiCad 0), checked by switching the new rule off; after it, 71 of 71 agree.

## The boards that showed it

`tracemaker route BOARD -o OUT --work 20000000 --variants 8 --threads 8` on four `srj20` boards, judged beside the input's project files: `copper_edge_clearance`
in the engine and in KiCad (`srj20-violation-counts.json`: every violation type, engine and KiCad):

| board | engine after the fix | KiCad |
|---|---:|---:|
| sample031 | 65 | 65 |
| sample007 | 72 | 72 |
| sample191 | 69 | 69 |
| sample039 | 78 | 78 |

(The issue gave 68 against 65 for sample031 before the fix, from the evaluation of PR #41; the other boards' counts before the fix were not measured again.) The two
types the engine does not check (`lib_footprint_issues`, `solder_mask_bridge`) are the only other differences on these boards. The routes themselves were not compared with
the earlier ones: the router reads only the `unconnected` list of the DRC, which this change does not touch.

## Not measured

Zone fills and copper graphics at zero clearance (they keep the touching rule); arcs of other radii; a clearance above zero for shapes other than a straight track; the
outline's own arcs; other KiCad versions.
