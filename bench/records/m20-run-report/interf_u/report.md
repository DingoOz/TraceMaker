# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-08T22:48:28Z[^3]. Input build/demos/interf\_u//unrouted.kicad\_pcb[^4], output bench/results/run-report/interf\_u/routed.kicad\_pcb[^5].

> **12[^6] of 164[^7] connections are open; 92.68[^8] % are routed.**

> **After the zones are refilled for the new copper, 17[^9] items are unconnected: the board is not finished. See [What is open and why](#what-is-open-and-why).**

> **4[^10] thermal reliefs are starved by the new copper after the refill: KiCad's DRC reports them as starved\_thermal, and this report's DRC counts do not include them. See [Zone refill](#zone-refill).**

The route added 0[^11] DRC errors of types routing can cause.

Next: 24 pins are open through congestion: more search (--work, --variants) or more copper layers (--try-layers 2,4 routes each count and compares them)[^12]

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 164 | `/connections` |
| Routed | 152 | `/routed` |
| Open | 12 | `/open` |
| Routed, % | 92.68 | `/completion_pct` |
| Unconnected after zone refill | 17 | `/unconnected_after_refill` |
| Tracks added | 613 | `/tracks` |
| Vias added | 90 | `/vias` |
| Copper layers | 2 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | rescue (fresh): exact bends, jittered order | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.1 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 3507062 | `/expansions` |
| Search expansions, all variants and passes | 66489549 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 213.284 | `/seconds` |

## What is open and why

Diagnosis of the open pins: an exact proof, past the board's fixed copper only, of whether a track or via can leave each pin at all. A pin it does not prove walled in is open through congestion: the copper routed so far is in its way.

|  | Value | JSON |
| --- | --- | --- |
| Pins of open connections | 24 | `/why_open/pins` |
| Walled in by fixed copper (no router connects them as placed) | 0 | `/why_open/walled_in` |
| Out only with a track or via smaller than the net class's | 0 | `/why_open/neck_down` |
| Walled in only by zone fills or copper graphics | 0 | `/why_open/soft_only` |
| Open through congestion | 24 | `/why_open/congested` |

Open connections by the router's cause (what failed when it last tried):

| Cause | Connections | What it means |
| --- | --- | --- |
| not\_attempted | 12 | No search was left for it: the budget (work or time) ended before the router tried it again, or, after the relax rescue, the last round, which keeps only the routes that are legal under the board's rules and does not search, left it open. The Why column says which. |

Rows: `/stuck/unrouted_by_reason/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U9 | 11 |
| U5 | 5 |
| U2 | 4 |
| BUS1 | 2 |
| U1 | 1 |
| U3 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each connection the router left open, in the router's order, with its last explanation and the nets in the way (those that blocked its search or, when none is named, those with the most routed copper round its two pads):

| Net | From | To | Distance between the pads, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| VCC | U1 pad 20 | BUS1 pad 3 | 25.048 | not\_attempted | its route of the relaxed round broke the board's clearance; the legalising round keeps only legal routes and does not search | crowded by /PC-RST, /PC-DB1, /PC-DB2, /PC-DB3, /PC-DB4, /PC-DB5 |
| /PC-A4 | U2 pad 17 | U9 pad H3 | 54.625 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by VCC, /PC-A7, /PC-A0, /MA7, /PC-A10, /PC-A3 |
| /PC-A1 | U3 pad 2 | U9 pad N4 | 35.966 | not\_attempted | its route of the relaxed round broke the board's clearance; the legalising round keeps only legal routes and does not search | crowded by /PC-A2, /PC-A0, /D0, /PC-A3, /PC-A10, /ENBBUF |
| /PC-A6 | U2 pad 15 | U9 pad J1 | 45.045 | not\_attempted | its route of the relaxed round broke the board's clearance; the legalising round keeps only legal routes and does not search | crowded by VCC, /PC-A7, /PC-A0, /PC-A3, /PC-A10, Net-(U9-P/A3) |
| /PC-A9 | BUS1 pad 53 | U9 pad N7 | 58.379 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /PC-AEN, /PC-A10, /PC-A2, VCC, /PC-A3, /PC-A0 |
| /OE- | U5 pad 24 | U9 pad B9 | 56.583 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /MD7, /MD6, /MA9, /MD5, /MD4, /MD3 |
| /MA15 | U5 pad 31 | U9 pad F1 | 56.64 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /MA14, /MA11, /MA13, /MA6, /MA9, /MA4 |
| /PC-A8 | U2 pad 4 | U9 pad H1 | 55.678 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /PC-A0, VCC, /PC-A7, /PC-A2, /PC-A3, /MA7 |
| /PC-A5 | U2 pad 2 | U9 pad J2 | 59.704 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /PC-A0, VCC, /PC-A7, /PC-A2, /PC-A3, /PC-A10 |
| /MA16 | U5 pad 2 | U9 pad C8 | 71.222 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /MA2, /MA3, /MD0, /MA4, /MD2, /MA1 |
| /MA5 | U5 pad 7 | U9 pad C13 | 71.222 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /MA2, /MA3, /MD0, /MA4, /MD2, /MA1 |
| /WR- | U5 pad 29 | U9 pad A13 | 78.996 | not\_attempted | open in the relaxed rounds too; the legalising round keeps only legal routes and does not search | crowded by /MA9, /MD7, /MD6, /MD5, /MD4, /MD3 |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (17[^13] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it. The DRC names its own two ends for each gap, so they need not be the pads of the router's open connection.

Of these, 12[^14] are on nets the router left a connection of open (listed above) and 5[^15] on nets the router completed, cut off by the zone refill.

| Missing connection | From | To | X of From, mm | Y of From, mm | Net left open by the router |
| --- | --- | --- | --- | --- | --- |
| Missing connection in net VCC | Pad 1 of C6 \[VCC\] | Pad 3 of BUS1 \[VCC\] | 159.385 | 111.76 | yes |
| Missing connection in net GND | Pad 2 of C6 \[GND\] | Pad 10 of U2 \[GND\] | 174.385 | 111.76 | no |
| Missing connection in net GND | Pad 10 of U2 \[GND\] | Pad 19 of U3 \[GND\] | 118.745 | 124.46 | no |
| Missing connection in net GND | Pad 19 of U3 \[GND\] | Pad 16 of U5 \[GND\] | 126.365 | 116.84 | no |
| Missing connection in net GND | Pad 16 of U5 \[GND\] | Pad 1 of U8 \[GND\] | 125.095 | 76.2 | no |
| Missing connection in net GND | Pad 1 of U8 \[GND\] | Pad K3 of U9 \[GND\] | 123.825 | 112.903 | no |
| Missing connection in net /PC-A5 | Pad 2 of U2 \[/PC-A5\] | Pad J2 of U9 \[/PC-A5\] | 98.425 | 124.46 | yes |
| Missing connection in net /PC-A8 | Pad 4 of U2 \[/PC-A8\] | Pad H1 of U9 \[/PC-A8\] | 103.505 | 124.46 | yes |
| Missing connection in net /PC-A9 | Pad 6 of U2 \[/PC-A9\] | Pad N7 of U9 \[/PC-A9\] | 108.585 | 124.46 | yes |
| Missing connection in net /PC-A6 | Pad 15 of U2 \[/PC-A6\] | Pad J1 of U9 \[/PC-A6\] | 108.585 | 116.84 | yes |
| Missing connection in net /PC-A4 | Pad 17 of U2 \[/PC-A4\] | Pad H3 of U9 \[/PC-A4\] | 103.505 | 116.84 | yes |
| Missing connection in net /PC-A1 | Pad 2 of U3 \[/PC-A1\] | Pad N4 of U9 \[/PC-A1\] | 126.365 | 124.46 | yes |
| Missing connection in net /MA16 | Pad 2 of U5 \[/MA16\] | Pad C8 of U9 \[/MA16\] | 89.535 | 76.2 | yes |
| Missing connection in net /MA5 | Pad 7 of U5 \[/MA5\] | Pad C13 of U9 \[/MA5\] | 102.235 | 76.2 | yes |
| Missing connection in net /OE- | Pad 24 of U5 \[/OE-\] | Pad B9 of U9 \[/OE-\] | 107.315 | 60.96 | yes |
| Missing connection in net /WR- | Pad 29 of U5 \[/WR-\] | Pad A13 of U9 \[/WR-\] | 94.615 | 60.96 | yes |
| Missing connection in net /MA15 | Pad 31 of U5 \[/MA15\] | Pad F1 of U9 \[/MA15\] | 89.535 | 60.96 | yes |

Rows: `/drc/after/unconnected_items/<row>`

## Relax rescue

The first pass left connections open, so the board was routed again with relax, route, legalise: first under clearances lowered below the board's (the relaxed stages; their counts are not legal routes), then one round under the board's own rules that keeps only what is legal. A pass is kept only if its legal result beats the one before. Routed before the rescue: 140[^16]; a rescue result kept: yes[^17].

| Pass | Routed under the board's rules | Kept | Seconds | Relaxed stages: clearance lowered by, mm, and routed |
| --- | --- | --- | --- | --- |
| warm | 149 | yes | 40.129 | 0.102: 151 |
| fresh | 152 | yes | 36.487 | 0.102: 155 |

Rows: `/rescue/passes/<row>`

## Zone refill

After the route the router refilled 1[^18] zones for the new copper; 22[^19] items were then unconnected.

Refill repair: the connections the refill cut off were routed again, with the first pass's work budget and what was left of the time limit; it is kept only if fewer items are unconnected after refilling once more.

|  | Value | JSON |
| --- | --- | --- |
| Connections tried | 22 | `/refill/repair/connections` |
| Routed | 5 | `/refill/repair/routed` |
| Tracks | 12 | `/refill/repair/tracks` |
| Vias | 5 | `/refill/repair/vias` |
| Search expansions | 4341361 | `/refill/repair/expansions` |
| Unconnected after refilling again | 17 | `/refill/repair/unconnected` |
| Kept | yes | `/refill/repair/kept` |

Thermal repair: for each starved relief, the nets whose new copper crosses its spokes were routed again with the spokes kept clear; kept pad by pad when fewer reliefs are starved and nothing more is unconnected.

|  | Value | JSON |
| --- | --- | --- |
| Starved before the repair | 5 | `/refill/thermal_repair/starved` |
| Pads tried | 5 | `/refill/thermal_repair/pads_tried` |
| Pads repaired | 1 | `/refill/thermal_repair/pads_repaired` |
| Nets routed again | 1 | `/refill/thermal_repair/nets_rerouted` |
| Starved after | 4 | `/refill/thermal_repair/starved_after` |
| Unconnected after | 17 | `/refill/thermal_repair/unconnected` |

> **4[^20] thermal reliefs are starved by the new copper: the pad's spokes no longer reach the pour (KiCad: starved\_thermal). The DRC counts of this report do not include them.**

| Pad | Net | Layer | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| U5.16 | GND | B.Cu | 125.095 | 76.2 |
| U9.C10 | GND | B.Cu | 165.735 | 72.39 |
| U9.K11 | GND | B.Cu | 168.275 | 90.17 |
| U9.L7 | GND | B.Cu | 158.115 | 92.71 |

Rows: `/refill/starved_thermal_pads/<row>`

## Rules

| File | Name | Found |
| --- | --- | --- |
| Project | unrouted.kicad\_pro | yes |
| Custom rules | unrouted.kicad\_dru | no |

Rows: `/rules/files`

The readers reported nothing they could not read.

Board minimums (the board setup's constraints). For each net the router uses the larger of the board minimum and its net class's value: the class's track and via are what it routes with, and it narrows a track or via only down to the minimum, where a pin needs it.

|  | Value | JSON |
| --- | --- | --- |
| Clearance, mm | 0 (not set) | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.2 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.9 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.5 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 (not set) | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0 (not set) | `/rules/minimums/copper_edge_clearance_mm` |
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

Critical nets: 2[^21] nets, 0[^22] vias, 6[^23] crossings of a reference-plane void.

Layer changes without a return via within reach: 0[^24] (before the stitching pass of --si-rules: 0[^25]).

Void crossings counts the voids of the reference plane a net's tracks run over; a split is a void that cuts the plane in two, counted on its own and named first in the warnings. The return detour is the extra way the return current takes round the voids, so it can be longer than the net's own track. The warnings name each void whose detour is over 2[^26] mm, so the listed detours need not add up to the total. New track is the length of the net's new copper, routed connections only.

| Net | Tier | Why critical | Vias | New track, mm | Open connections | Void crossings | Splits | Return detour, mm | Unstitched layer changes | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Net-(U9-P/A3) | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 30.3 | 0 | 4 | 0 | 5.731 | 0 | runs over 4 voids of its reference plane (detour 5.73 mm), detour of 2.36 mm round a void on F.Cu at (112.457, 92.386) mm, detour of 2.44 mm round a void on F.Cu at (131.135, 92.790) mm |
| /8MH-OUT | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 15.766 | 0 | 2 | 0 | 3.13 | 0 | runs over 2 voids of its reference plane (detour 3.13 mm) |

Rows: `/critical_nets/nets/<row>`

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^27] (KiCad's violation names); zones refilled first: yes[^28]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 164 | 17 |

Rows: `/drc/before, /drc/after`

Unconnected items are counted as KiCad counts them, one per link missing from a net's copper. The router plans its own connections (with --soft-zones it does not count the old zone fills as joining pads), so the count before the route need not equal its connections.

Errors added of types routing can cause: 0[^29]; violations gone: 0[^30].

## Options and seed

|  | Value | JSON |
| --- | --- | --- |
| Seed | 1 | `/run/seed` |
| Work budget of each variant in each pass, expansions | 3000000 | `/run/work_budget` |
| Time limit, s | 3600 | `/run/time_limit_s` |
| Stopped by the time limit | no | `/time_limited` |
| Variants | 8 | `/run/variants` |
| Threads | 4 | `/run/threads` |

The work budget bounds each variant of each pass (the first pass, each rescue pass, the refill repair), not the run as a whole; the time limit bounds the first pass, each rescue pass again, and the repairs share what is left of it.

Options given (command line and --config):

| Option | Value |
| --- | --- |
| --output | bench/results/run-report/interf\_u/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --no-kb | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-host | 0.0.0.0 |
| --view-port | 8811 |
| --json | bench/results/run-report/interf\_u/summary.json |
| --report | bench/results/run-report/interf\_u/report.html |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 117 | 13.863 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 132 | 16.201 | no |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 23 | 13.703 | no |  |
| first pass | 3 | fast bends, jittered order, escape plan | 140 | 17.302 | yes |  |
| first pass | 4 | exact bends, jittered order | 108 | 10.216 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 131 | 14.156 | no |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 132 | 13.801 | no |  |
| first pass | 7 | fast bends, dear vias, crowded source | 130 | 13.497 | no |  |
| rescue (warm) | 0 | exact bends, shortest first | 149 | 3.852 | no | yes |
| rescue (warm) | 1 | fast bends, shortest first, crowded source | 149 | 3.671 | no | yes |
| rescue (warm) | 2 | fast bends, longest first (2x pitch on large boards) | 149 | 3.515 | no | yes |
| rescue (warm) | 3 | fast bends, jittered order, escape plan | 149 | 3.944 | no | yes |
| rescue (warm) | 4 | exact bends, jittered order | 149 | 3.244 | yes | yes |
| rescue (warm) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 149 | 3.231 | no | yes |
| rescue (warm) | 6 | fast bends, cheap crossings, escape plan, crowded source | 149 | 3.383 | no | yes |
| rescue (warm) | 7 | fast bends, dear vias, crowded source | 149 | 3.382 | no | yes |
| rescue (fresh) | 0 | exact bends, shortest first | 152 | 3.233 | no | yes |
| rescue (fresh) | 1 | fast bends, shortest first, crowded source | 152 | 3.313 | no | yes |
| rescue (fresh) | 2 | fast bends, longest first (2x pitch on large boards) | 152 | 3.403 | no | yes |
| rescue (fresh) | 3 | fast bends, jittered order, escape plan | 152 | 3.293 | no | yes |
| rescue (fresh) | 4 | exact bends, jittered order | 152 | 3.202 | yes | yes |
| rescue (fresh) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 152 | 3.242 | no | yes |
| rescue (fresh) | 6 | fast bends, cheap crossings, escape plan, crowded source | 152 | 3.273 | no | yes |
| rescue (fresh) | 7 | fast bends, dear vias, crowded source | 152 | 3.355 | no | yes |

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
[^10]: `/refill/starved_thermals`
[^11]: `/drc/added_routing_errors`
[^12]: `/why_open/next/0`
[^13]: `/drc/after/unconnected`
[^14]: `/drc/after/unconnected_open_by_router`
[^15]: `/drc/after/unconnected_cut_by_refill`
[^16]: `/rescue/routed_before`
[^17]: `/rescue/adopted`
[^18]: `/refill/zones`
[^19]: `/refill/unconnected`
[^20]: `/refill/starved_thermals`
[^21]: `/critical_nets/totals/nets`
[^22]: `/critical_nets/totals/vias`
[^23]: `/critical_nets/totals/void_crossings`
[^24]: `/critical_nets/totals/missing_stitches`
[^25]: `/critical_nets/totals/missing_stitches_before_stitching`
[^26]: `/critical_nets/detour_listed_over_mm`
[^27]: `/drc/checker`
[^28]: `/drc/zones_refilled`
[^29]: `/drc/added_routing_errors`
[^30]: `/drc/removed`
