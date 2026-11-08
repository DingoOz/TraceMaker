# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-08T22:46:05Z[^3]. Input build/demos/plane\_smd//unrouted.kicad\_pcb[^4], output bench/results/run-report/plane\_smd/routed.kicad\_pcb[^5].

> **1[^6] of 49[^7] connections are open; 97.96[^8] % are routed.**

> **After the zones are refilled for the new copper, 1[^9] item is unconnected: the board is not finished. See [What is open and why](#what-is-open-and-why).**

The route added 0[^10] DRC errors of types routing can cause.

Next: 1 pin gets out only with a track or via smaller than the net class's (the board's minimums allow them; see each pin's text): check that the net is not in --no-neck-nets, or lower the class's track width or via size[^11]

Next: 1 pin is open through congestion: more search (--work, --variants) or more copper layers (--try-layers 2,4 routes each count and compares them)[^12]

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 49 | `/connections` |
| Routed | 48 | `/routed` |
| Open | 1 | `/open` |
| Routed, % | 97.96 | `/completion_pct` |
| Unconnected after zone refill | 1 | `/unconnected_after_refill` |
| Tracks added | 107 | `/tracks` |
| Vias added | 38 | `/vias` |
| Copper layers | 4 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.025 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 3003647 | `/expansions` |
| Search expansions, all variants and passes | 66828313 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 70.792 | `/seconds` |

## What is open and why

Diagnosis of the open pins: an exact proof, past the board's fixed copper only, of whether a track or via can leave each pin at all. A pin it does not prove walled in is open through congestion: the copper routed so far is in its way.

|  | Value | JSON |
| --- | --- | --- |
| Pins of open connections | 2 | `/why_open/pins` |
| Walled in by fixed copper (no router connects them as placed) | 0 | `/why_open/walled_in` |
| Out only with a track or via smaller than the net class's | 1 | `/why_open/neck_down` |
| Walled in only by zone fills or copper graphics | 0 | `/why_open/soft_only` |
| Open through congestion | 1 | `/why_open/congested` |

These pins get out only with a track or via smaller than their net class's; the board's minimums allow it:

| Pin | Net | What would help |
| --- | --- | --- |
| U2.B2 | INT | U2.B2 (INT) is walled in on F.Cu for its class's 0.1 mm track and 0.45 mm via (U2.A2, U2.B1, U2.B3, U2.C2, U2.A1, U2.A3), but a via of 0.25 mm fits beside it. |

Rows: `/why_open/neck_pins/<row>`

Open connections by the router's cause (what failed when it last tried):

| Cause | Connections | What it means |
| --- | --- | --- |
| boxed\_in | 1 | When the router last tried, no track could leave a pad of the connection on its routing lattice: the copper round it (pads, keep-outs, and the tracks and vias routed by then) left no way out. Whether fixed copper alone does that is the diagnosis above: a pin it does not prove walled in is open through congestion, and moving routed copper can free it. |

Rows: `/stuck/unrouted_by_reason/<row>`

The package analysis's coarse lattice found no way out for these pins either, but the exact proof finds one, so they are not counted as trapped:

| Part | Pads |
| --- | --- |
| U2 | B2 |

Rows: `/stuck/escape_analysis/<row>/not_proved`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U1 | 1 |
| U2 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each connection the router left open, in the router's order, with its last explanation and the nets in the way (those that blocked its search or, when none is named, those with the most routed copper round its two pads):

| Net | From | To | Distance between the pads, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| INT | U1 pad 21 | U2 pad B2 | 5.663 | boxed\_in | boxed in; escapes/neck-down: skipped: identical earlier attempt failed (nogood) | crowded by CS, MOSI, MISO, SCK, GND, +3V3 |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (1[^13] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it. The DRC names its own two ends for each gap, so they need not be the pads of the router's open connection.

Of these, 1[^14] are on nets the router left a connection of open (listed above) and 0[^15] on nets the router completed, cut off by the zone refill.

| Missing connection | From | To | X of From, mm | Y of From, mm | Net left open by the router |
| --- | --- | --- | --- | --- | --- |
| Missing connection in net INT | Pad 21 of U1 \[INT\] | Pad B2 of U2 \[INT\] | 111.438 | 106.75 | yes |

Rows: `/drc/after/unconnected_items/<row>`

## Relax rescue

The first pass left connections open, so the board was routed again with relax, route, legalise: first under clearances lowered below the board's (the relaxed stages; their counts are not legal routes), then one round under the board's own rules that keeps only what is legal. A pass is kept only if its legal result beats the one before. Routed before the rescue: 48[^16]; a rescue result kept: no[^17].

| Pass | Routed under the board's rules | Kept | Seconds | Relaxed stages: clearance lowered by, mm, and routed |
| --- | --- | --- | --- | --- |
| warm | 48 | no | 27.778 | 0.04: 48 |
| fresh | 46 | no | 19.615 | 0.04: 48 |

Rows: `/rescue/passes/<row>`

## Zone refill

After the route the router refilled 2[^18] zones for the new copper; 1[^19] items were then unconnected.

Refill repair: the connections the refill cut off were routed again, with the first pass's work budget and what was left of the time limit; it is kept only if fewer items are unconnected after refilling once more.

|  | Value | JSON |
| --- | --- | --- |
| Connections tried | 1 | `/refill/repair/connections` |
| Routed | 0 | `/refill/repair/routed` |
| Tracks | 0 | `/refill/repair/tracks` |
| Vias | 0 | `/refill/repair/vias` |
| Search expansions | 809390 | `/refill/repair/expansions` |
| Unconnected after refilling again | 1 | `/refill/repair/unconnected` |
| Kept | no | `/refill/repair/kept` |

Thermal reliefs starved by the new copper: 0[^20].

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

Critical nets: 2[^21] nets, 0[^22] vias, 0[^23] crossings of a reference-plane void.

Layer changes without a return via within reach: 0[^24] (before the stitching pass of --si-rules: 0[^25]).

Void crossings counts the voids of the reference plane a net's tracks run over; a split is a void that cuts the plane in two, counted on its own and named first in the warnings. The return detour is the extra way the return current takes round the voids, so it can be longer than the net's own track. The warnings name each void whose detour is over 2[^26] mm, so the listed detours need not add up to the total. New track is the length of the net's new copper, routed connections only.

| Net | Tier | Why critical | Vias | New track, mm | Open connections | Void crossings | Splits | Return detour, mm | Unstitched layer changes | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| XOUT | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 6.35 | 0 | 0 | 0 | 0 | 0 |  |
| XIN | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 11.436 | 0 | 0 | 0 | 0 | 0 |  |

Rows: `/critical_nets/nets/<row>`

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^27] (KiCad's violation names); zones refilled first: yes[^28]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 47 | 1 |

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
| --output | bench/results/run-report/plane\_smd/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --no-kb | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-host | 0.0.0.0 |
| --view-port | 8814 |
| --json | bench/results/run-report/plane\_smd/summary.json |
| --report | bench/results/run-report/plane\_smd/report.html |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 33 | 6.809 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 48 | 10.131 | no |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 45 | 9.241 | no |  |
| first pass | 3 | fast bends, jittered order, escape plan | 48 | 11.548 | no |  |
| first pass | 4 | exact bends, jittered order | 33 | 4.199 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 48 | 10.629 | yes |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 48 | 10.494 | no |  |
| first pass | 7 | fast bends, dear vias, crowded source | 48 | 10.958 | no |  |
| rescue (warm) | 0 | exact bends, shortest first | 48 | 0.04 | no | no |
| rescue (warm) | 1 | fast bends, shortest first, crowded source | 48 | 0.031 | no | no |
| rescue (warm) | 2 | fast bends, longest first (2x pitch on large boards) | 48 | 0.042 | yes | no |
| rescue (warm) | 3 | fast bends, jittered order, escape plan | 48 | 0.084 | no | no |
| rescue (warm) | 4 | exact bends, jittered order | 48 | 0.028 | no | no |
| rescue (warm) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 48 | 0.047 | no | no |
| rescue (warm) | 6 | fast bends, cheap crossings, escape plan, crowded source | 48 | 0.025 | no | no |
| rescue (warm) | 7 | fast bends, dear vias, crowded source | 48 | 0.016 | no | no |
| rescue (fresh) | 0 | exact bends, shortest first | 46 | 0.353 | no | no |
| rescue (fresh) | 1 | fast bends, shortest first, crowded source | 46 | 0.233 | no | no |
| rescue (fresh) | 2 | fast bends, longest first (2x pitch on large boards) | 46 | 0.254 | yes | no |
| rescue (fresh) | 3 | fast bends, jittered order, escape plan | 46 | 0.209 | no | no |
| rescue (fresh) | 4 | exact bends, jittered order | 46 | 0.272 | no | no |
| rescue (fresh) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 46 | 0.273 | no | no |
| rescue (fresh) | 6 | fast bends, cheap crossings, escape plan, crowded source | 46 | 0.173 | no | no |
| rescue (fresh) | 7 | fast bends, dear vias, crowded source | 46 | 0.216 | no | no |

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
[^12]: `/why_open/next/1`
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
