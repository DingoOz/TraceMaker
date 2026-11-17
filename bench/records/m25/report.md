# Run report: plane\_smd.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-17T13:57:29Z[^3]. Input plane\_smd.kicad\_pcb[^4], output plane\_smd.routed.kicad\_pcb[^5].

> **6[^6] of 47[^7] connections are open; 87.23[^8] % are routed.**

The route added 0[^9] DRC errors of types routing can cause.

Next: route again with --soft-zones: 1 pin is walled in only by zone fills[^10]

Next: 10 pins are open through congestion: more search (--work, --variants) or more copper layers (--try-layers 2,4 routes each count and compares them)[^11]

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 47 | `/connections` |
| Routed | 41 | `/routed` |
| Open | 6 | `/open` |
| Routed, % | 87.23 | `/completion_pct` |
| Tracks added | 223 | `/tracks` |
| Vias added | 0 | `/vias` |
| Copper layers | 4 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.025 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 3110969 | `/expansions` |
| Search expansions, all variants and passes | 49305727 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 13.18 | `/seconds` |

## What is open and why

Diagnosis of the open pins: an exact proof, past the board's fixed copper only, of whether a track or via can leave each pin at all. A pin it does not prove walled in is open through congestion: the copper routed so far is in its way.

|  | Value | JSON |
| --- | --- | --- |
| Pins of open connections | 11 | `/why_open/pins` |
| Walled in by fixed copper (no router connects them as placed) | 0 | `/why_open/walled_in` |
| Out only with a track or via smaller than the net class's | 0 | `/why_open/neck_down` |
| Walled in only by zone fills or copper graphics | 1 | `/why_open/soft_only` |
| Open through congestion | 10 | `/why_open/congested` |

Open connections by the router's cause (what failed when it last tried):

| Cause | Connections | What it means |
| --- | --- | --- |
| ripped\_not\_rerouted | 5 | It was routed, then ripped up to make room for another net, and could not be routed again. |
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
| U2 | 6 |
| U1 | 4 |
| C6 | 1 |
| U3 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each connection the router left open, in the router's order, with its last explanation and the nets in the way (those that blocked its search or, when none is named, those with the most routed copper round its two pads):

| Net | From | To | Distance between the pads, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| GND | U2 pad B1 | U2 pad C3 | 0.894 | ripped\_not\_rerouted | ripped up by SCK and not routed again | +3V3 |
| GND | C6 pad 2 | U1 pad 33 | 4.304 | ripped\_not\_rerouted | ripped up by SCK and not routed again | crowded by +3V3, SCK, CS, SDA |
| GND | U2 pad B1 | U3 pad 2 | 4.203 | ripped\_not\_rerouted | ripped up by SCK and not routed again | +3V3 |
| INT | U1 pad 21 | U2 pad B2 | 5.663 | boxed\_in | boxed in; escapes/neck-down: skipped: identical earlier attempt failed (nogood); negotiated: boxed in (target); given up: boxed in on the routing lattice, even with routed copper negotiable | crowded by +3V3, GND, SCK, CS, SDA |
| MISO | U1 pad 19 | U2 pad A3 | 6.211 | ripped\_not\_rerouted | ripped up by MOSI and not routed again | crowded by +3V3, GND, SCK, CS, SDA, SCL |
| MOSI | U1 pad 18 | U2 pad A4 | 6.722 | ripped\_not\_rerouted | ripped up by CS and not routed again | MISO |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (6[^12] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it. The DRC names its own two ends for each gap, so they need not be the pads of the router's open connection.

Of these, 6[^13] are on nets the router left a connection of open (listed above) and 0[^14] on nets the router completed, cut off by the zone refill.

| Missing connection | From | To | X of From, mm | Y of From, mm | Net left open by the router |
| --- | --- | --- | --- | --- | --- |
| Missing connection in net GND | Pad 2 of C6 \[GND\] | Pad 2 of C2 \[GND\] | 112.8 | 109.02 | yes |
| Missing connection in net GND | Pad 2 of C2 \[GND\] | Pad 2 of C7 \[GND\] | 104.4 | 105.12 | yes |
| Missing connection in net GND | Pad 2 of C7 \[GND\] | Pad 2 of U3 \[GND\] | 117.98 | 109.6 | yes |
| Missing connection in net MOSI | Pad 18 of U1 \[MOSI\] | Pad A4 of U2 \[MOSI\] | 111.438 | 108.25 | yes |
| Missing connection in net MISO | Pad 19 of U1 \[MISO\] | Pad A3 of U2 \[MISO\] | 111.438 | 107.75 | yes |
| Missing connection in net INT | Pad 21 of U1 \[INT\] | Pad B2 of U2 \[INT\] | 111.438 | 106.75 | yes |

Rows: `/drc/after/unconnected_items/<row>`

## Relax rescue

The first pass left connections open, so the board was routed again with relax, route, legalise: first under clearances lowered below the board's (the relaxed stages; their counts are not legal routes), then one round under the board's own rules that keeps only what is legal. A pass is kept only if its legal result beats the one before. Routed before the rescue: 41[^15]; a rescue result kept: no[^16].

| Pass | Routed under the board's rules | Kept | Seconds | Relaxed stages: clearance lowered by, mm, and routed |
| --- | --- | --- | --- | --- |
| warm | 39 | no | 4.797 | 0.04: 43 |
| fresh | 39 | no | 4.373 | 0.04: 42 |

Rows: `/rescue/passes/<row>`

## Zone refill

The zones were not refilled by the router (no --soft-zones, or no copper zones).

## Rules

| File | Name | Found |
| --- | --- | --- |
| Project | plane\_smd.kicad\_pro | yes |
| Custom rules | plane\_smd.kicad\_dru | no |

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

These critical nets were found from the component rules' roles and the differential pairs, and audited after the route; the route did not treat them apart (--critical-auto and the options of doc 05 §44 do; --no-critical-audit skips this).

No return-path fault: no audited net crosses a split, every layer change has a return via or capacitor within reach, and no detour is over the listed threshold.

Critical nets: 2[^17] nets, 0[^18] vias, 0[^19] crossings of a reference-plane void.

Layer changes without a return via within reach: 0[^20].

Void crossings counts the voids of the reference plane a net's tracks run over; a split is a void that cuts the plane in two, counted on its own and named first in the warnings. The return detour is the extra way the return current takes round the voids, so it can be longer than the net's own track. The warnings name each void whose detour is over 2[^21] mm, so the listed detours need not add up to the total. New track is the length of the net's new copper, routed connections only.

| Net | Tier | Why critical | Vias | New track, mm | Open connections | Void crossings | Splits | Return detour, mm | Unstitched layer changes | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| XOUT | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 7.728 | 0 | 0 | 0 | 0 | 0 |  |
| XIN | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 11.252 | 0 | 0 | 0 | 0 | 0 |  |

Rows: `/critical_nets/nets/<row>`

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^22] (KiCad's violation names); zones refilled first: yes[^23]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 47 | 6 |

Rows: `/drc/before, /drc/after`

Unconnected items are counted as KiCad counts them, one per link missing from a net's copper. The router plans its own connections (with --soft-zones it does not count the old zone fills as joining pads), so the count before the route need not equal its connections.

Errors added of types routing can cause: 0[^24]; violations gone: 0[^25].

## Options and seed

|  | Value | JSON |
| --- | --- | --- |
| Seed | 1 | `/run/seed` |
| Work budget of each variant in each pass, expansions | 3000000 | `/run/work_budget` |
| Time limit, s | 120 | `/run/time_limit_s` |
| Stopped by the time limit | no | `/time_limited` |
| Variants | 8 | `/run/variants` |
| Threads | 8 | `/run/threads` |

The work budget bounds each variant of each pass (the first pass, each rescue pass, the refill repair), not the run as a whole; the time limit bounds the first pass, each rescue pass again, and the repairs share what is left of it.

Options given (command line and --config):

| Option | Value |
| --- | --- |
| --output | plane\_smd.routed.kicad\_pcb |
| --work | 3000000 |
| --no-kb | true |
| --view | true |
| --view-port | 9951 |
| --json | plane\_smd.json |
| --report | plane\_smd.report.md |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 39 | 2.417 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 41 | 3.697 | no |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 36 | 3.305 | no |  |
| first pass | 3 | fast bends, jittered order, escape plan | 40 | 3.28 | no |  |
| first pass | 4 | exact bends, jittered order | 36 | 2.562 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 41 | 3.984 | yes |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 40 | 3.64 | no |  |
| first pass | 7 | fast bends, dear vias, crowded source | 41 | 3.675 | no |  |
| rescue (warm) | 0 | exact bends, shortest first | 39 | 0.264 | no | no |
| rescue (warm) | 1 | fast bends, shortest first, crowded source | 39 | 0.262 | no | no |
| rescue (warm) | 2 | fast bends, longest first (2x pitch on large boards) | 39 | 0.273 | no | no |
| rescue (warm) | 3 | fast bends, jittered order, escape plan | 39 | 0.254 | yes | no |
| rescue (warm) | 4 | exact bends, jittered order | 39 | 0.263 | no | no |
| rescue (warm) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 39 | 0.264 | no | no |
| rescue (warm) | 6 | fast bends, cheap crossings, escape plan, crowded source | 39 | 0.266 | no | no |
| rescue (warm) | 7 | fast bends, dear vias, crowded source | 39 | 0.265 | no | no |
| rescue (fresh) | 0 | exact bends, shortest first | 39 | 0.115 | no | no |
| rescue (fresh) | 1 | fast bends, shortest first, crowded source | 39 | 0.116 | no | no |
| rescue (fresh) | 2 | fast bends, longest first (2x pitch on large boards) | 39 | 0.119 | no | no |
| rescue (fresh) | 3 | fast bends, jittered order, escape plan | 39 | 0.124 | no | no |
| rescue (fresh) | 4 | exact bends, jittered order | 39 | 0.116 | yes | no |
| rescue (fresh) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 39 | 0.117 | no | no |
| rescue (fresh) | 6 | fast bends, cheap crossings, escape plan, crowded source | 39 | 0.135 | no | no |
| rescue (fresh) | 7 | fast bends, dear vias, crowded source | 39 | 0.119 | no | no |

Rows: `/variants/<row>`

---

Every number and name in this report is a field of `plane_smd.json`; the notes give the JSON pointer of each value in the text.

[^1]: `/run/board`
[^2]: `/run/version`
[^3]: `/run/date`
[^4]: `/run/input`
[^5]: `/run/output`
[^6]: `/open`
[^7]: `/connections`
[^8]: `/completion_pct`
[^9]: `/drc/added_routing_errors`
[^10]: `/why_open/next/0`
[^11]: `/why_open/next/1`
[^12]: `/drc/after/unconnected`
[^13]: `/drc/after/unconnected_open_by_router`
[^14]: `/drc/after/unconnected_cut_by_refill`
[^15]: `/rescue/routed_before`
[^16]: `/rescue/adopted`
[^17]: `/critical_nets/totals/nets`
[^18]: `/critical_nets/totals/vias`
[^19]: `/critical_nets/totals/void_crossings`
[^20]: `/critical_nets/totals/missing_stitches`
[^21]: `/critical_nets/detour_listed_over_mm`
[^22]: `/drc/checker`
[^23]: `/drc/zones_refilled`
[^24]: `/drc/added_routing_errors`
[^25]: `/drc/removed`
