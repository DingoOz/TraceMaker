# M21 B3 records: `--si-rules` closes the audit loop

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 57, decision D144, doc 18 section 14. Branch `m20-c5-fanout`,
2026-10-27/28.

## Files

| File | What |
|---|---|
| `runs/stitch/<board>/` | `tracemaker route unrouted.kicad_pcb --work 3000000 --time 3600 --threads 4 --soft-zones --si-rules --no-kb`: `summary.json` (`--json`) and the log |
| `runs/nostitch/<board>/` | the same with `--no-si-stitch` |

The plane tier's eight boards (`bench/prepare_demos.py --set planes`), routed directly, without KiCad judging. The five-seed runs
with KiCad judging (arms `si` and `sistitch`) are in `bench/records/m21-a5`.

## Results

| Board | Routed | Critical nets | Unstitched changes, before the pass → after | Return vias added |
|---|--:|--:|---|--:|
| interf_u | 140/164 | 2 | 0 → 0 | 0 |
| plane_smd | 48/49 | 2 | 0 → 0 | 0 |
| RoyalBlue54L-Feather | 120/147 | 6 | 0 → 0 | 0 |
| StickHub | 127/128 | 18 | 1 → 1 (no plane copper on one side) | 0 |
| complex_hierarchy, multichannel_mixer, pic_programmer, sonde xilinx | as without the option | 0 | | |

Both runs route the same copper (`--no-si-stitch` changes only the pass). In the five-seed sweep (`bench/records/m21-a5`), every seed
of `--si-rules` leaves the same single StickHub change, with or without the pass, and adds no KiCad error beyond the three boards
every arm has. Option sets other than `--si-rules` leave 2 to 7 unstitched changes. So the `--si-rules` routing avoids them, and on
these boards the pass has nothing to do.

The pass is shown working on a constructed board: test `[pour][route][critical]` "route --si-rules stitches the critical nets' layer
changes the audit finds without a return via" (one change before, a GND via beside it after, none in the audit). The audit's fallback
for a stub over its own antipad (doc 05 §57) is covered by the same test.
