# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.10.0[^2], 2026-10-24T01:47:09Z[^3]. Input build/demos/plane\_smd//unrouted.kicad\_pcb[^4], output bench/results/run-report/plane\_smd/routed.kicad\_pcb[^5].

> **1[^6] of 49[^7] connections are open; 97.96[^8] % are routed.**

> **After the zones are refilled for the new copper, 1[^9] items are unconnected: the board is not finished. See What is open and why.**

The route added 0[^10] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 49 | `/connections` |
| Routed | 48 | `/routed` |
| Open | 1 | `/open` |
| Routed, % | 97.96 | `/completion_pct` |
| Unconnected after zone refill | 1 | `/unconnected_after_refill` |
| Tracks added | 111 | `/tracks` |
| Vias added | 38 | `/vias` |
| Winning variant | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | `/variant_name` |
| Lattice pitch, mm | 0.025 | `/pitch_mm` |
| Search expansions, all variants | 22226970 | `/work_all_variants/total/expansions` |
| Wall time, s | 94.713 | `/seconds` |

## What is open and why

Open connections by cause:

| Cause | Connections | What it means |
| --- | --- | --- |
| boxed\_in | 1 | Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it on the layers it can use. |

Rows: `/stuck/unrouted_by_reason/<row>`

> **Some pins cannot leave their package at all under the board's rules; no amount of routing effort connects them.**

| Part | Pitch, mm | Pins that cannot escape | Pads | Why | What would help |
| --- | --- | --- | --- | --- | --- |
| U2 | 0.4 | 1 | B2 | no channel at 0.100 mm and no via site within reach |  |

Rows: `/stuck/escape_analysis/<row>/dead/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U1 | 1 |
| U2 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each open connection, with the router's last explanation and the nets whose copper was in the way:

| Net | From | To | Length, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| INT | U1 pad 21 | U2 pad B2 | 5.663 | boxed\_in | boxed in (target); escapes/neck-down: skipped: identical earlier attempt failed (nogood) |  |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (1[^11] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it.

| Missing connection | From | To | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| Missing connection in net INT | Pad 21 of U1 \[INT\] | Pad B2 of U2 \[INT\] | 111.438 | 106.75 |

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
| Clearance, mm | 0.1 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.1 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.25 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.15 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0.2 | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0.3 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 11 | 0.1 | 0.1 | 0.45 | 0.2 | 0.2 | 0.25 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

Critical nets: 2[^12] nets, 0[^13] vias, 0[^14] crossings of a reference-plane void.

| Net | Tier | Why critical | Vias | Length, mm | Unrouted | Void crossings | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- |
| XOUT | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 6.346 | 0 | 0 |  |
| XIN | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 11.187 | 0 | 0 |  |

Rows: `/critical_nets/nets/<row>`

## DRC before and after

Checked with tracemaker drc[^15] (KiCad's violation names); zones refilled first: yes[^16]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 47 | 1 |

Rows: `/drc/before, /drc/after`

Errors added of types routing can cause: 0[^17]; violations gone: 0[^18].

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
| --output | bench/results/run-report/plane\_smd/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8813 |
| --json | bench/results/run-report/plane\_smd/summary.json |
| --report | bench/results/run-report/plane\_smd/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 6 | 11.941 |
| 1 | fast bends, shortest first | 48 | 17.89 |
| 2 | fast bends, longest first (2x pitch on large boards) | 44 | 39.209 |
| 3 | fast bends, jittered order, escape plan | 48 | 17.507 |
| 4 | exact bends, jittered order | 6 | 16.753 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 48 | 18.227 |
| 6 | fast bends, cheap crossings, escape plan | 48 | 61.659 |
| 7 | fast bends, dear vias | 48 | 16.096 |

Rows: `/variants/<row>`

---

Every number and name in this report is a field of `summary.json`; the notes give the JSON pointer of each value in the text.

[^1]: `/run/board`
[^2]: `/run/version`
[^3]: `/run/date`
[^4]: `/run/input`
[^5]: `/run/output`
[^6]: `/open`
[^7]: `/connections`
[^8]: `/completion_pct`
[^9]: `/unconnected_after_refill`
[^10]: `/drc/added_routing_errors`
[^11]: `/drc/after/unconnected`
[^12]: `/critical_nets/totals/nets`
[^13]: `/critical_nets/totals/vias`
[^14]: `/critical_nets/totals/void_crossings`
[^15]: `/drc/checker`
[^16]: `/drc/zones_refilled`
[^17]: `/drc/added_routing_errors`
[^18]: `/drc/removed`
