# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.10.0[^2], 2026-10-24T01:47:13Z[^3]. Input build/demos/pic\_programmer//unrouted.kicad\_pcb[^4], output bench/results/run-report/pic\_programmer/routed.kicad\_pcb[^5].

> **4[^6] of 86[^7] connections are open; 95.35[^8] % are routed.**

> **After the zones are refilled for the new copper, 7[^9] items are unconnected: the board is not finished. See What is open and why.**

The route added 0[^10] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 86 | `/connections` |
| Routed | 82 | `/routed` |
| Open | 4 | `/open` |
| Routed, % | 95.35 | `/completion_pct` |
| Unconnected after zone refill | 7 | `/unconnected_after_refill` |
| Tracks added | 219 | `/tracks` |
| Vias added | 2 | `/vias` |
| Winning variant | fast bends, dear vias | `/variant_name` |
| Lattice pitch, mm | 0.1 | `/pitch_mm` |
| Search expansions, all variants | 18175618 | `/work_all_variants/total/expansions` |
| Wall time, s | 98.275 | `/seconds` |

## What is open and why

Open connections by cause:

| Cause | Connections | What it means |
| --- | --- | --- |
| boxed\_in | 4 | Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it on the layers it can use. |

Rows: `/stuck/unrouted_by_reason/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| JP1 | 4 |
| Q3 | 2 |
| P3 | 1 |
| R21 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each open connection, with the router's last explanation and the nets whose copper was in the way:

| Net | From | To | Length, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| VCC | Q3 pad 3 | JP1 pad 1 | 2.577 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target); negotiated: boxed in (target); fixed copper encloses the pin |  |
| /pic\_sockets/VCC\_PIC | JP1 pad 2 | R21 pad 2 | 4.526 | boxed\_in | boxed in; escapes/neck-down: boxed in; negotiated: boxed in; fixed copper encloses the pin |  |
| /pic\_sockets/VCC\_PIC | JP1 pad 2 | Q3 pad 1 | 6.567 | boxed\_in | boxed in; escapes/neck-down: boxed in; negotiated: boxed in; fixed copper encloses the pin |  |
| /pic\_sockets/VCC\_PIC | P3 pad 11 | JP1 pad 2 | 34.145 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target); negotiated: boxed in (target); negotiated escapes/neck-down: out of budget |  |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (7[^11] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it.

| Missing connection | From | To | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| Missing connection in net VCC | Pad 1 of C1 \[VCC\] | Pad 1 of JP1 \[VCC\] | 110.49 | 78.867 |
| Missing connection in net GND | Pad 2 of C1 \[GND\] | Pad 5 of U5 \[GND\] | 85.49 | 78.867 |
| Missing connection in net GND | Pad 5 of U5 \[GND\] | Pad 4 of U2 \[GND\] | 207.899 | 118.11 |
| Missing connection in net GND | Pad 4 of U2 \[GND\] | Pad 13 of U2 \[GND\] | 123.19 | 119.38 |
| Missing connection in net /pic\_sockets/VCC\_PIC | Pad 11 of P3 \[/pic\_sockets/VCC\_PIC\] | Pad 1 of Q3 \[/pic\_sockets/VCC\_PIC\] | 175.26 | 76.2 |
| Missing connection in net /pic\_sockets/VCC\_PIC | Pad 1 of Q3 \[/pic\_sockets/VCC\_PIC\] | Pad 2 of R21 \[/pic\_sockets/VCC\_PIC\] | 142.24 | 97.79 |
| Missing connection in net /pic\_sockets/VCC\_PIC | Pad 2 of R21 \[/pic\_sockets/VCC\_PIC\] | Pad 2 of JP1 \[/pic\_sockets/VCC\_PIC\] | 147.955 | 102.235 |

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
| Clearance, mm | 0 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.25 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.9 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.5 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0.01 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 109 | 0.25 | 0.5 | 1.6 | 0.6 | 0.25 | 0.25 |
| POWER | 2 | 0.28 | 0.8 | 1.6 | 0.6 | 0.28 | 0.28 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

The options that turn the audit on were given, but the board has nothing for them: no critical nets, differential pairs, buses or return-path nets were found.

## DRC before and after

Checked with tracemaker drc[^12] (KiCad's violation names); zones refilled first: yes[^13]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 86 | 7 |

Rows: `/drc/before, /drc/after`

Errors added of types routing can cause: 0[^14]; violations gone: 0[^15].

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
| --output | bench/results/run-report/pic\_programmer/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8812 |
| --json | bench/results/run-report/pic\_programmer/summary.json |
| --report | bench/results/run-report/pic\_programmer/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 0 | 9.43 |
| 1 | fast bends, shortest first | 82 | 17.017 |
| 2 | fast bends, longest first (2x pitch on large boards) | 80 | 76.162 |
| 3 | fast bends, jittered order, escape plan | 82 | 17.616 |
| 4 | exact bends, jittered order | 0 | 7.178 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 82 | 14.439 |
| 6 | fast bends, cheap crossings, escape plan | 82 | 66.836 |
| 7 | fast bends, dear vias | 82 | 13.561 |

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
[^12]: `/drc/checker`
[^13]: `/drc/zones_refilled`
[^14]: `/drc/added_routing_errors`
[^15]: `/drc/removed`
