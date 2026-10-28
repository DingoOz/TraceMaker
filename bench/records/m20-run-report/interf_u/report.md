# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.11.0[^2], 2026-10-27T22:57:42Z[^3]. Input build/demos/interf\_u/unrouted.kicad\_pcb[^4], output build/scratch/d3/interf\_u/routed.kicad\_pcb[^5].

> **24[^6] of 164[^7] connections are open; 85.37[^8] % are routed.**

> **After the zones are refilled for the new copper, 26[^9] items are unconnected: the board is not finished. See What is open and why.**

The route added 0[^10] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 164 | `/connections` |
| Routed | 140 | `/routed` |
| Open | 24 | `/open` |
| Routed, % | 85.37 | `/completion_pct` |
| Unconnected after zone refill | 26 | `/unconnected_after_refill` |
| Tracks added | 529 | `/tracks` |
| Vias added | 41 | `/vias` |
| Winning variant | fast bends, jittered order, escape plan | `/variant_name` |
| Lattice pitch, mm | 0.1 | `/pitch_mm` |
| Search expansions, all variants | 22274548 | `/work_all_variants/total/expansions` |
| Wall time, s | 128.028 | `/seconds` |

## What is open and why

Open connections by cause:

| Cause | Connections | What it means |
| --- | --- | --- |
| not\_attempted | 22 | The run ended (work or time) before the router tried it again. |
| boxed\_in | 2 | Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it on the layers it can use. |

Rows: `/stuck/unrouted_by_reason/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U9 | 20 |
| U5 | 13 |
| BUS1 | 5 |
| U2 | 5 |
| U3 | 3 |
| R4 | 1 |
| U1 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each open connection, with the router's last explanation and the nets whose copper was in the way:

| Net | From | To | Length, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| VCC | U1 pad 20 | BUS1 pad 3 | 25.048 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) |  |
| /PC-A3 | U3 pad 5 | BUS1 pad 59 | 35.854 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) |  |
| /PC-A4 | U2 pad 17 | U9 pad H3 | 54.625 | not\_attempted |  |  |
| /MD5 | U5 pad 19 | U9 pad A8 | 41.133 | not\_attempted |  |  |
| /PC-A10 | U2 pad 11 | U9 pad L8 | 48.36 | not\_attempted |  |  |
| /PC-A0 | U3 pad 3 | BUS1 pad 62 | 38.206 | not\_attempted |  |  |
| /PC-A0 | U3 pad 3 | U9 pad M6 | 39.554 | not\_attempted |  |  |
| /OE- | U5 pad 24 | U9 pad B9 | 56.583 | not\_attempted |  |  |
| /MA0 | U5 pad 12 | U9 pad A11 | 54.076 | not\_attempted |  |  |
| /PC-AEN | U2 pad 1 | BUS1 pad 42 | 50.241 | not\_attempted |  |  |
| /MA8 | U5 pad 27 | U9 pad G1 | 48.277 | not\_attempted |  |  |
| /MA14 | U5 pad 3 | U9 pad D1 | 50.816 | not\_attempted |  |  |
| /MA6 | U5 pad 6 | U9 pad F2 | 45.878 | not\_attempted |  |  |
| /MD6 | U5 pad 20 | U9 pad A9 | 46.159 | not\_attempted |  |  |
| /MA12 | U5 pad 4 | U9 pad F3 | 53.476 | not\_attempted |  |  |
| /PC-A8 | U2 pad 4 | U9 pad H1 | 55.678 | not\_attempted |  |  |
| /MA13 | U5 pad 28 | U9 pad C1 | 47.127 | not\_attempted |  |  |
| /MA7 | U5 pad 5 | U9 pad B3 | 51.195 | not\_attempted |  |  |
| /MA16 | U5 pad 2 | U9 pad C8 | 71.222 | not\_attempted |  |  |
| /LED1 | R4 pad 2 | U9 pad M3 | 53.476 | not\_attempted |  |  |
| /PC-A9 | BUS1 pad 53 | U9 pad N7 | 58.379 | not\_attempted |  |  |
| /WR- | U5 pad 29 | U9 pad A13 | 78.996 | not\_attempted |  |  |
| /PC-A5 | U2 pad 2 | U9 pad J2 | 59.704 | not\_attempted |  |  |
| /MA5 | U5 pad 7 | U9 pad C13 | 71.222 | not\_attempted |  |  |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (26[^11] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it.

| Missing connection | From | To | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| Missing connection in net VCC | Pad 1 of C6 \[VCC\] | Pad 3 of BUS1 \[VCC\] | 159.385 | 111.76 |
| Missing connection in net GND | Pad 2 of C6 \[GND\] | Pad 12 of U8 \[GND\] | 174.385 | 111.76 |
| Missing connection in net /PC-AEN | Pad 1 of U2 \[/PC-AEN\] | Pad N10 of U9 \[/PC-AEN\] | 95.885 | 124.46 |
| Missing connection in net /PC-A5 | Pad 2 of U2 \[/PC-A5\] | Pad J2 of U9 \[/PC-A5\] | 98.425 | 124.46 |
| Missing connection in net /PC-A8 | Pad 4 of U2 \[/PC-A8\] | Pad H1 of U9 \[/PC-A8\] | 103.505 | 124.46 |
| Missing connection in net /PC-A9 | Pad 6 of U2 \[/PC-A9\] | Pad N7 of U9 \[/PC-A9\] | 108.585 | 124.46 |
| Missing connection in net /PC-A10 | Pad 11 of U2 \[/PC-A10\] | Pad L8 of U9 \[/PC-A10\] | 118.745 | 116.84 |
| Missing connection in net /PC-A4 | Pad 17 of U2 \[/PC-A4\] | Pad H3 of U9 \[/PC-A4\] | 103.505 | 116.84 |
| Missing connection in net /PC-A0 | Pad 3 of U3 \[/PC-A0\] | Pad M6 of U9 \[/PC-A0\] | 128.905 | 124.46 |
| Missing connection in net /PC-A0 | Pad M6 of U9 \[/PC-A0\] | Pad 62 of BUS1 \[/PC-A0\] | 155.575 | 95.25 |
| Missing connection in net /PC-A3 | Pad 5 of U3 \[/PC-A3\] | Pad 59 of BUS1 \[/PC-A3\] | 133.985 | 124.46 |
| Missing connection in net /MA16 | Pad 2 of U5 \[/MA16\] | Pad C8 of U9 \[/MA16\] | 89.535 | 76.2 |
| Missing connection in net /MA14 | Pad 3 of U5 \[/MA14\] | Pad D1 of U9 \[/MA14\] | 92.075 | 76.2 |
| Missing connection in net /MA12 | Pad 4 of U5 \[/MA12\] | Pad F3 of U9 \[/MA12\] | 94.615 | 76.2 |
| Missing connection in net /MA7 | Pad 5 of U5 \[/MA7\] | Pad B3 of U9 \[/MA7\] | 97.155 | 76.2 |
| Missing connection in net /MA6 | Pad 6 of U5 \[/MA6\] | Pad F2 of U9 \[/MA6\] | 99.695 | 76.2 |
| Missing connection in net /MA5 | Pad 7 of U5 \[/MA5\] | Pad C13 of U9 \[/MA5\] | 102.235 | 76.2 |
| Missing connection in net /MA0 | Pad 12 of U5 \[/MA0\] | Pad A11 of U9 \[/MA0\] | 114.935 | 76.2 |
| Missing connection in net /MD5 | Pad 19 of U5 \[/MD5\] | Pad A8 of U9 \[/MD5\] | 120.015 | 60.96 |
| Missing connection in net /MD6 | Pad 20 of U5 \[/MD6\] | Pad A9 of U9 \[/MD6\] | 117.475 | 60.96 |
| Missing connection in net /OE- | Pad 24 of U5 \[/OE-\] | Pad B9 of U9 \[/OE-\] | 107.315 | 60.96 |
| Missing connection in net /MA11 | Pad 25 of U5 \[/MA11\] | Pad D2 of U9 \[/MA11\] | 104.775 | 60.96 |
| Missing connection in net /MA8 | Pad 27 of U5 \[/MA8\] | Pad G1 of U9 \[/MA8\] | 99.695 | 60.96 |
| Missing connection in net /MA13 | Pad 28 of U5 \[/MA13\] | Pad C1 of U9 \[/MA13\] | 97.155 | 60.96 |
| Missing connection in net /WR- | Pad 29 of U5 \[/WR-\] | Pad A13 of U9 \[/WR-\] | 94.615 | 60.96 |
| Missing connection in net /LED1 | Pad 2 of R4 \[/LED1\] | Pad M3 of U9 \[/LED1\] | 144.145 | 41.91 |

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
| Track width, mm | 0.2 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.9 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.5 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 171 | 0.254 | 0.4 | 1.4 | 0.6 | 0.2 | 0.25 |
| Power | 2 | 0.254 | 0.5 | 1.6 | 0.6 | 0.2 | 0.25 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

Critical nets: 2[^12] nets, 0[^13] vias, 6[^14] crossings of a reference-plane void.

| Net | Tier | Why critical | Vias | Length, mm | Unrouted | Void crossings | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Net-(U9-P{slash}A3) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 30.305 | 0 | 4 | crosses 1 split of its reference plane |
| /8MH-OUT | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 15.766 | 0 | 2 | crosses 1 split of its reference plane |

Rows: `/critical_nets/nets/<row>`

## DRC before and after

Checked with tracemaker drc[^15] (KiCad's violation names); zones refilled first: yes[^16]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 164 | 26 |

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
| --output | build/scratch/d3/interf\_u/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8840 |
| --json | build/scratch/d3/interf\_u/summary.json |
| --report | build/scratch/d3/interf\_u/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 117 | 10.412 |
| 1 | fast bends, shortest first | 137 | 14.489 |
| 2 | fast bends, longest first (2x pitch on large boards) | 23 | 7.733 |
| 3 | fast bends, jittered order, escape plan | 140 | 15.326 |
| 4 | exact bends, jittered order | 109 | 9.415 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 137 | 13.396 |
| 6 | fast bends, cheap crossings, escape plan | 137 | 12.074 |
| 7 | fast bends, dear vias | 136 | 13.306 |

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
