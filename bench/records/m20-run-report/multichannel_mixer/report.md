# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-08T22:46:16Z[^3]. Input build/demos/multichannel\_mixer//unrouted.kicad\_pcb[^4], output bench/results/run-report/multichannel\_mixer/routed.kicad\_pcb[^5].

The router joined every connection it planned: 173[^6] of 173[^7].

> **After the zones are refilled for the new copper, 3[^8] items are unconnected: the board is not finished. See [What is open and why](#what-is-open-and-why).**

> **1[^9] thermal relief is starved by the new copper after the refill: KiCad's DRC reports them as starved\_thermal, and this report's DRC counts do not include them. See [Zone refill](#zone-refill).**

The route added 0[^10] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 173 | `/connections` |
| Routed | 173 | `/routed` |
| Open | 0 | `/open` |
| Routed, % | 100 | `/completion_pct` |
| Unconnected after zone refill | 3 | `/unconnected_after_refill` |
| Tracks added | 459 | `/tracks` |
| Vias added | 30 | `/vias` |
| Copper layers | 2 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | fast bends, dear vias, crowded source | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.1 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 3050476 | `/expansions` |
| Search expansions, all variants and passes | 22326356 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 81.686 | `/seconds` |

## What is open and why

The router joined every connection it planned.

Unconnected on the board as written, by the DRC after the route (3[^11] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it. The DRC names its own two ends for each gap, so they need not be the pads of the router's open connection.

Of these, 0[^12] are on nets the router left a connection of open (listed above) and 3[^13] on nets the router completed, cut off by the zone refill.

| Missing connection | From | To | X of From, mm | Y of From, mm | Net left open by the router |
| --- | --- | --- | --- | --- | --- |
| Missing connection in net vbias | Pad 1 of C2 \[vbias\] | Pad 3 of IC5 \[vbias\] | 187.73 | 101.73 | no |
| Missing connection in net vbias | Pad 3 of IC5 \[vbias\] | Pad 3 of IC4 \[vbias\] | 103 | 81.4 | no |
| Missing connection in net vbias | Pad 3 of IC4 \[vbias\] | Pad 3 of IC3 \[vbias\] | 169 | 81.4 | no |

Rows: `/drc/after/unconnected_items/<row>`

## Relax rescue

No rescue pass ran: the first pass left nothing open, or the rescue was turned off.

## Zone refill

After the route the router refilled 2[^14] zones for the new copper; 8[^15] items were then unconnected.

Refill repair: the connections the refill cut off were routed again, with the first pass's work budget and what was left of the time limit; it is kept only if fewer items are unconnected after refilling once more.

|  | Value | JSON |
| --- | --- | --- |
| Connections tried | 8 | `/refill/repair/connections` |
| Routed | 5 | `/refill/repair/routed` |
| Tracks | 2 | `/refill/repair/tracks` |
| Vias | 5 | `/refill/repair/vias` |
| Search expansions | 2136755 | `/refill/repair/expansions` |
| Unconnected after refilling again | 3 | `/refill/repair/unconnected` |
| Kept | yes | `/refill/repair/kept` |

Thermal repair: for each starved relief, the nets whose new copper crosses its spokes were routed again with the spokes kept clear; kept pad by pad when fewer reliefs are starved and nothing more is unconnected.

|  | Value | JSON |
| --- | --- | --- |
| Starved before the repair | 1 | `/refill/thermal_repair/starved` |
| Pads tried | 1 | `/refill/thermal_repair/pads_tried` |
| Pads repaired | 0 | `/refill/thermal_repair/pads_repaired` |
| Nets routed again | 0 | `/refill/thermal_repair/nets_rerouted` |
| Starved after | 1 | `/refill/thermal_repair/starved_after` |
| Unconnected after | 3 | `/refill/thermal_repair/unconnected` |

> **1[^16] thermal relief is starved by the new copper: the pad's spokes no longer reach the pour (KiCad: starved\_thermal). The DRC counts of this report do not include them.**

| Pad | Net | Layer | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| C2.2 | GND | B.Cu | 185.19 | 101.73 |

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

Critical nets were looked for (from the component rules' roles, such as clocks, crystals and USB, and from named differential pairs) and the board has none, so the reference-return audit and the stitching of --si-rules had nothing to check.

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^17] (KiCad's violation names); zones refilled first: yes[^18]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 173 | 3 |

Rows: `/drc/before, /drc/after`

Unconnected items are counted as KiCad counts them, one per link missing from a net's copper. The router plans its own connections (with --soft-zones it does not count the old zone fills as joining pads), so the count before the route need not equal its connections.

Errors added of types routing can cause: 0[^19]; violations gone: 0[^20].

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
| --output | bench/results/run-report/multichannel\_mixer/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --no-kb | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-host | 0.0.0.0 |
| --view-port | 8812 |
| --json | bench/results/run-report/multichannel\_mixer/summary.json |
| --report | bench/results/run-report/multichannel\_mixer/report.html |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 171 | 13.308 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 173 | 17.849 | no |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 173 | 20.277 | no |  |
| first pass | 3 | fast bends, jittered order, escape plan | 173 | 20.089 | no |  |
| first pass | 4 | exact bends, jittered order | 171 | 7.932 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 173 | 18.563 | no |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 173 | 16.541 | no |  |
| first pass | 7 | fast bends, dear vias, crowded source | 173 | 15.728 | yes |  |

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
[^9]: `/refill/starved_thermals`
[^10]: `/drc/added_routing_errors`
[^11]: `/drc/after/unconnected`
[^12]: `/drc/after/unconnected_open_by_router`
[^13]: `/drc/after/unconnected_cut_by_refill`
[^14]: `/refill/zones`
[^15]: `/refill/unconnected`
[^16]: `/refill/starved_thermals`
[^17]: `/drc/checker`
[^18]: `/drc/zones_refilled`
[^19]: `/drc/added_routing_errors`
[^20]: `/drc/removed`
