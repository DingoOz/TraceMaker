# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.10.0[^2], 2026-10-24T01:48:32Z[^3]. Input build/demos/multichannel\_mixer//unrouted.kicad\_pcb[^4], output bench/results/run-report/multichannel\_mixer/routed.kicad\_pcb[^5].

The router joined every connection it planned: 173[^6] of 173[^7].

> **After the zones are refilled for the new copper, 3[^8] items are unconnected: the board is not finished. See What is open and why.**

The route added 0[^9] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 173 | `/connections` |
| Routed | 173 | `/routed` |
| Open | 0 | `/open` |
| Routed, % | 100 | `/completion_pct` |
| Unconnected after zone refill | 3 | `/unconnected_after_refill` |
| Tracks added | 457 | `/tracks` |
| Vias added | 30 | `/vias` |
| Winning variant | fast bends, dear vias | `/variant_name` |
| Lattice pitch, mm | 0.1 | `/pitch_mm` |
| Search expansions, all variants | 22711535 | `/work_all_variants/total/expansions` |
| Wall time, s | 180.309 | `/seconds` |

## What is open and why

The router joined every connection it planned.

Unconnected on the board as written, by the DRC after the route (3[^10] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it.

| Missing connection | From | To | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| Missing connection in net vbias | Pad 1 of C2 \[vbias\] | Pad 3 of IC5 \[vbias\] | 187.73 | 101.73 |
| Missing connection in net vbias | Pad 3 of IC5 \[vbias\] | Pad 3 of IC4 \[vbias\] | 103 | 81.4 |
| Missing connection in net vbias | Pad 3 of IC4 \[vbias\] | Pad 3 of IC3 \[vbias\] | 169 | 81.4 |

Rows: `/drc/after/unconnected_items/<row>`

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
| Clearance, mm | 0.3 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.3 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 1.5 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.3 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.3 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0.25 | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0.5 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 80 | 0.2 | 0.2 | 0.6 | 0.3 | 0.2 | 0.25 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

The options that turn the audit on were given, but the board has nothing for them: no critical nets, differential pairs, buses or return-path nets were found.

## DRC before and after

Checked with tracemaker drc[^11] (KiCad's violation names); zones refilled first: yes[^12]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 173 | 3 |

Rows: `/drc/before, /drc/after`

Errors added of types routing can cause: 0[^13]; violations gone: 0[^14].

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
| --output | bench/results/run-report/multichannel\_mixer/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8811 |
| --json | bench/results/run-report/multichannel\_mixer/summary.json |
| --report | bench/results/run-report/multichannel\_mixer/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 171 | 52.312 |
| 1 | fast bends, shortest first | 173 | 34.215 |
| 2 | fast bends, longest first (2x pitch on large boards) | 173 | 165.47 |
| 3 | fast bends, jittered order, escape plan | 173 | 37.108 |
| 4 | exact bends, jittered order | 171 | 107.101 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 173 | 25.927 |
| 6 | fast bends, cheap crossings, escape plan | 173 | 118.159 |
| 7 | fast bends, dear vias | 173 | 19.81 |

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
[^10]: `/drc/after/unconnected`
[^11]: `/drc/checker`
[^12]: `/drc/zones_refilled`
[^13]: `/drc/added_routing_errors`
[^14]: `/drc/removed`
