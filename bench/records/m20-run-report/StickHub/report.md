# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-08T22:46:58Z[^3]. Input build/demos/StickHub//unrouted.kicad\_pcb[^4], output bench/results/run-report/StickHub/routed.kicad\_pcb[^5].

> **1[^6] of 128[^7] connections are open; 99.22[^8] % are routed.**

> **After the zones are refilled for the new copper, 4[^9] items are unconnected: the board is not finished. See [What is open and why](#what-is-open-and-why).**

The route added 0[^10] DRC errors of types routing can cause.

Next: 2 pins are open through congestion: more search (--work, --variants) or more copper layers (--try-layers 2,4 routes each count and compares them)[^11]

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 128 | `/connections` |
| Routed | 127 | `/routed` |
| Open | 1 | `/open` |
| Routed, % | 99.22 | `/completion_pct` |
| Unconnected after zone refill | 4 | `/unconnected_after_refill` |
| Tracks added | 683 | `/tracks` |
| Vias added | 78 | `/vias` |
| Copper layers | 2 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | fast bends, cheap crossings, escape plan, crowded source | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.05 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 3080158 | `/expansions` |
| Search expansions, all variants and passes | 52851565 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 123.717 | `/seconds` |

## What is open and why

Diagnosis of the open pins: an exact proof, past the board's fixed copper only, of whether a track or via can leave each pin at all. A pin it does not prove walled in is open through congestion: the copper routed so far is in its way.

|  | Value | JSON |
| --- | --- | --- |
| Pins of open connections | 2 | `/why_open/pins` |
| Walled in by fixed copper (no router connects them as placed) | 0 | `/why_open/walled_in` |
| Out only with a track or via smaller than the net class's | 0 | `/why_open/neck_down` |
| Walled in only by zone fills or copper graphics | 0 | `/why_open/soft_only` |
| Open through congestion | 2 | `/why_open/congested` |

Open connections by the router's cause (what failed when it last tried):

| Cause | Connections | What it means |
| --- | --- | --- |
| ripped\_not\_rerouted | 1 | It was routed, then ripped up to make room for another net, and could not be routed again. |

Rows: `/stuck/unrouted_by_reason/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| J6 | 1 |
| U1 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each connection the router left open, in the router's order, with its last explanation and the nets in the way (those that blocked its search or, when none is named, those with the most routed copper round its two pads):

| Net | From | To | Distance between the pads, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| /U5D- | J6 pad 2 | U1 pad 32 | 3.208 | ripped\_not\_rerouted | ripped up by /D- and not routed again | /D-, +3.3V |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (4[^12] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it. The DRC names its own two ends for each gap, so they need not be the pads of the router's open connection.

Of these, 1[^13] are on nets the router left a connection of open (listed above) and 3[^14] on nets the router completed, cut off by the zone refill.

| Missing connection | From | To | X of From, mm | Y of From, mm | Net left open by the router |
| --- | --- | --- | --- | --- | --- |
| Missing connection in net GND | Pad 1 of D4 \[GND\] | Pad 10 of U1 \[GND\] | 150.4 | 96.3 | no |
| Missing connection in net GND | Pad 10 of U1 \[GND\] | Pad 39 of U1 \[GND\] | 154.931 | 97.044 | no |
| Missing connection in net +5V | Pad 1 of J7 \[+5V\] | Pad 1 of J5 \[+5V\] | 148.15 | 96.25 | no |
| Missing connection in net /U5D- | Pad 2 of D9 \[/U5D-\] | Pad 32 of U1 \[/U5D-\] | 149.6 | 104.2 | yes |

Rows: `/drc/after/unconnected_items/<row>`

## Relax rescue

The first pass left connections open, so the board was routed again with relax, route, legalise: first under clearances lowered below the board's (the relaxed stages; their counts are not legal routes), then one round under the board's own rules that keeps only what is legal. A pass is kept only if its legal result beats the one before. Routed before the rescue: 127[^15]; a rescue result kept: no[^16].

| Pass | Routed under the board's rules | Kept | Seconds | Relaxed stages: clearance lowered by, mm, and routed |
| --- | --- | --- | --- | --- |
| warm | 121 | no | 37.583 | 0.06: 128 |
| fresh | 120 | no | 32.672 | 0.06: 128 |

Rows: `/rescue/passes/<row>`

## Zone refill

After the route the router refilled 5[^17] zones for the new copper; 18[^18] items were then unconnected.

Refill repair: the connections the refill cut off were routed again, with the first pass's work budget and what was left of the time limit; it is kept only if fewer items are unconnected after refilling once more.

|  | Value | JSON |
| --- | --- | --- |
| Connections tried | 18 | `/refill/repair/connections` |
| Routed | 14 | `/refill/repair/routed` |
| Tracks | 19 | `/refill/repair/tracks` |
| Vias | 14 | `/refill/repair/vias` |
| Search expansions | 3000809 | `/refill/repair/expansions` |
| Unconnected after refilling again | 4 | `/refill/repair/unconnected` |
| Kept | yes | `/refill/repair/kept` |

Thermal reliefs starved by the new copper: 0[^19].

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
| Clearance, mm | 0.15 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.15 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.5 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.1 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.25 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 (not set) | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0 (not set) | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 47 | 0.15 | 0.15 | 0.5 | 0.3 | 0.15 | 0.15 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

Critical nets: 18[^20] nets, 23[^21] vias, 67[^22] crossings of a reference-plane void.

Layer changes without a return via within reach: 1[^23] (before the stitching pass of --si-rules: 1[^24]).

Void crossings counts the voids of the reference plane a net's tracks run over; a split is a void that cuts the plane in two, counted on its own and named first in the warnings. The return detour is the extra way the return current takes round the voids, so it can be longer than the net's own track. The warnings name each void whose detour is over 2[^25] mm, so the listed detours need not add up to the total. New track is the length of the net's new copper, routed connections only.

| Net | Tier | Why critical | Vias | New track, mm | Open connections | Void crossings | Splits | Return detour, mm | Unstitched layer changes | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| /XI | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 3.409 | 0 | 1 | 1 | 0 | 0 | crosses 1 split of its reference plane |
| /XO | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 4.87 | 0 | 1 | 1 | 0 | 0 | crosses 1 split of its reference plane |
| /U2D+ | 2 | differential pair | 1 | 6.528 | 0 | 2 | 0 | 1.478 | 0 | runs over 2 voids of its reference plane (detour 1.48 mm) |
| /U6D- | 2 | differential pair | 1 | 5.261 | 0 | 3 | 1 | 9.168 | 0 | crosses 1 split of its reference plane, detour of 9.01 mm round a void on B.Cu at (146.747, 100.226) mm |
| /U6D+ | 2 | differential pair | 1 | 4.279 | 0 | 3 | 1 | 8.012 | 0 | crosses 1 split of its reference plane, detour of 8.00 mm round a void on B.Cu at (146.216, 100.227) mm |
| /U7D- | 2 | differential pair | 1 | 13.387 | 0 | 3 | 1 | 2.284 | 0 | crosses 1 split of its reference plane, detour of 2.28 mm round a void on B.Cu at (143.250, 96.570) mm |
| /U7D+ | 2 | differential pair | 3 | 14.327 | 0 | 7 | 1 | 3.057 | 0 | crosses 1 split of its reference plane |
| /U5D+ | 2 | differential pair | 1 | 7.283 | 0 | 4 | 2 | 0 | 0 | crosses 2 splits of its reference plane |
| /U4D+ | 2 | differential pair | 1 | 19.878 | 0 | 5 | 0 | 4.774 | 0 | runs over 5 voids of its reference plane (detour 4.77 mm), detour of 2.20 mm round a void on B.Cu at (155.974, 97.542) mm, detour of 2.11 mm round a void on B.Cu at (157.224, 96.376) mm |
| /U5D- | 2 | differential pair | 0 | 1.454 | 1 | 0 | 0 | 0 | 0 | 1 connection not routed |
| /U1D- | 2 | differential pair | 1 | 7.397 | 0 | 2 | 0 | 3.312 | 0 | runs over 2 voids of its reference plane (detour 3.31 mm), detour of 3.09 mm round a void on F.Cu at (151.660, 105.263) mm |
| /U3D- | 2 | differential pair | 1 | 11.847 | 0 | 4 | 4 | 0 | 0 | crosses 4 splits of its reference plane |
| /U2D- | 2 | differential pair | 1 | 5.373 | 0 | 4 | 1 | 7.113 | 0 | crosses 1 split of its reference plane, detour of 6.09 mm round a void on F.Cu at (150.224, 98.300) mm |
| /U1D+ | 2 | differential pair | 1 | 6.221 | 0 | 2 | 1 | 0 | 0 | crosses 1 split of its reference plane |
| /U4D- | 2 | differential pair | 3 | 23.454 | 0 | 7 | 2 | 1.281 | 0 | crosses 2 splits of its reference plane |
| /U3D+ | 2 | differential pair | 3 | 14.938 | 0 | 8 | 2 | 12.825 | 0 | crosses 2 splits of its reference plane, detour of 3.83 mm round a void on B.Cu at (156.596, 98.113) mm, detour of 8.65 mm round a void on B.Cu at (155.526, 98.474) mm |
| /D- | 2 | USB 2.0 (Full/High Speed) port (usb2 dm) | 1 | 15.814 | 0 | 5 | 1 | 2.446 | 1 | crosses 1 split of its reference plane, layer change F.Cu to B.Cu at (149.750, 111.600) mm: no reference copper on one side, detour of 2.12 mm round a void on B.Cu at (147.475, 107.225) mm |
| /D+ | 2 | USB 2.0 (Full/High Speed) port (usb2 dp) | 3 | 19.2 | 0 | 6 | 1 | 0.234 | 0 | crosses 1 split of its reference plane |

Rows: `/critical_nets/nets/<row>`

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^26] (KiCad's violation names); zones refilled first: yes[^27]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 128 | 4 |

Rows: `/drc/before, /drc/after`

Unconnected items are counted as KiCad counts them, one per link missing from a net's copper. The router plans its own connections (with --soft-zones it does not count the old zone fills as joining pads), so the count before the route need not equal its connections.

Errors added of types routing can cause: 0[^28]; violations gone: 0[^29].

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
| --output | bench/results/run-report/StickHub/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --no-kb | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-host | 0.0.0.0 |
| --view-port | 8817 |
| --json | bench/results/run-report/StickHub/summary.json |
| --report | bench/results/run-report/StickHub/report.html |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 70 | 7.2 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 126 | 13.548 | no |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 115 | 7.601 | no |  |
| first pass | 3 | fast bends, jittered order, escape plan | 116 | 7.814 | no |  |
| first pass | 4 | exact bends, jittered order | 72 | 6.376 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 118 | 17.377 | no |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 127 | 8.523 | yes |  |
| first pass | 7 | fast bends, dear vias, crowded source | 120 | 6.786 | no |  |
| rescue (warm) | 0 | exact bends, shortest first | 121 | 1.888 | no | no |
| rescue (warm) | 1 | fast bends, shortest first, crowded source | 121 | 1.887 | no | no |
| rescue (warm) | 2 | fast bends, longest first (2x pitch on large boards) | 121 | 2.006 | yes | no |
| rescue (warm) | 3 | fast bends, jittered order, escape plan | 121 | 2.085 | no | no |
| rescue (warm) | 4 | exact bends, jittered order | 121 | 1.66 | no | no |
| rescue (warm) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 121 | 1.795 | no | no |
| rescue (warm) | 6 | fast bends, cheap crossings, escape plan, crowded source | 121 | 1.967 | no | no |
| rescue (warm) | 7 | fast bends, dear vias, crowded source | 121 | 1.82 | no | no |
| rescue (fresh) | 0 | exact bends, shortest first | 120 | 2.631 | no | no |
| rescue (fresh) | 1 | fast bends, shortest first, crowded source | 120 | 2.214 | no | no |
| rescue (fresh) | 2 | fast bends, longest first (2x pitch on large boards) | 120 | 2.226 | yes | no |
| rescue (fresh) | 3 | fast bends, jittered order, escape plan | 119 | 2.572 | no | no |
| rescue (fresh) | 4 | exact bends, jittered order | 120 | 2.317 | no | no |
| rescue (fresh) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 120 | 2.419 | no | no |
| rescue (fresh) | 6 | fast bends, cheap crossings, escape plan, crowded source | 120 | 2.497 | no | no |
| rescue (fresh) | 7 | fast bends, dear vias, crowded source | 120 | 2.52 | no | no |

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
[^11]: `/why_open/next/0`
[^12]: `/drc/after/unconnected`
[^13]: `/drc/after/unconnected_open_by_router`
[^14]: `/drc/after/unconnected_cut_by_refill`
[^15]: `/rescue/routed_before`
[^16]: `/rescue/adopted`
[^17]: `/refill/zones`
[^18]: `/refill/unconnected`
[^19]: `/refill/starved_thermals`
[^20]: `/critical_nets/totals/nets`
[^21]: `/critical_nets/totals/vias`
[^22]: `/critical_nets/totals/void_crossings`
[^23]: `/critical_nets/totals/missing_stitches`
[^24]: `/critical_nets/totals/missing_stitches_before_stitching`
[^25]: `/critical_nets/detour_listed_over_mm`
[^26]: `/drc/checker`
[^27]: `/drc/zones_refilled`
[^28]: `/drc/added_routing_errors`
[^29]: `/drc/removed`
