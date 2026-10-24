# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.10.0[^2], 2026-10-24T01:46:05Z[^3]. Input build/demos/sonde xilinx//unrouted.kicad\_pcb[^4], output bench/results/run-report/sonde\_xilinx/routed.kicad\_pcb[^5].

Every connection was routed: 48[^6] of 48[^7].

After the zones are refilled for the new copper, 0[^8] items are unconnected.

The route added 0[^9] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 48 | `/connections` |
| Routed | 48 | `/routed` |
| Open | 0 | `/open` |
| Routed, % | 100 | `/completion_pct` |
| Unconnected after zone refill | 0 | `/unconnected_after_refill` |
| Tracks added | 125 | `/tracks` |
| Vias added | 2 | `/vias` |
| Winning variant | fast bends, dear vias | `/variant_name` |
| Lattice pitch, mm | 0.1 | `/pitch_mm` |
| Search expansions, all variants | 11423997 | `/work_all_variants/total/expansions` |
| Wall time, s | 28.59 | `/seconds` |

## What is open and why

Nothing is open.

## Rules

| File | Name | Found |
| --- | --- | --- |
| Project | unrouted.kicad\_pro | yes |
| Custom rules | unrouted.kicad\_dru | no |

Rows: `/rules/files`

The readers reported nothing they could not read.

Board minimums:

|  | Value | JSON |
| --- | --- | --- |
| Clearance, mm | 0 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.203 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.889 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.508 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0.01 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 42 | 0.254 | 0.635 | 1.651 | 0.635 | 0.4 | 0.4 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

The options that turn the audit on were given, but the board has nothing for them: no critical nets, differential pairs, buses or return-path nets were found.

## DRC before and after

Checked with tracemaker drc[^10] (KiCad's violation names); zones refilled first: yes[^11]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 48 | 0 |

Rows: `/drc/before, /drc/after`

Errors added of types routing can cause: 0[^12]; violations gone: 0[^13].

## Options and seed

|  | Value | JSON |
| --- | --- | --- |
| Seed | 1 | `/run/seed` |
| Work budget per variant, expansions | 3000000 | `/run/work_budget` |
| Time limit, s | 3600 | `/run/time_limit_s` |
| Stopped by the time limit | no | `/time_limited` |
| Variants | 8 | `/run/variants` |
| Threads | 4 | `/run/threads` |

Options given (command line and --config):

| Option | Value |
| --- | --- |
| --output | bench/results/run-report/sonde\_xilinx/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8815 |
| --json | bench/results/run-report/sonde\_xilinx/summary.json |
| --report | bench/results/run-report/sonde\_xilinx/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 48 | 25.213 |
| 1 | fast bends, shortest first | 48 | 3.355 |
| 2 | fast bends, longest first (2x pitch on large boards) | 48 | 17.708 |
| 3 | fast bends, jittered order, escape plan | 48 | 4.412 |
| 4 | exact bends, jittered order | 48 | 21.969 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 48 | 14.386 |
| 6 | fast bends, cheap crossings, escape plan | 48 | 7.713 |
| 7 | fast bends, dear vias | 48 | 3.81 |

Rows: `/variants/<row>`

---

Every number and name in this report is a field of `summary.json`; the notes give the JSON pointer of each value in the text.

[^1]: `/run/board`
[^2]: `/run/version`
[^3]: `/run/date`
[^4]: `/run/input`
[^5]: `/run/output`
[^6]: `/routed`
[^7]: `/connections`
[^8]: `/unconnected_after_refill`
[^9]: `/drc/added_routing_errors`
[^10]: `/drc/checker`
[^11]: `/drc/zones_refilled`
[^12]: `/drc/added_routing_errors`
[^13]: `/drc/removed`
