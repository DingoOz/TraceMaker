# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-08T22:46:15Z[^3]. Input build/demos/pic\_programmer//unrouted.kicad\_pcb[^4], output bench/results/run-report/pic\_programmer/routed.kicad\_pcb[^5].

> **4[^6] of 86[^7] connections are open; 95.35[^8] % are routed.**

> **After the zones are refilled for the new copper, 4[^9] items are unconnected: the board is not finished. See [What is open and why](#what-is-open-and-why).**

The route added 0[^10] DRC errors of types routing can cause.

Next: 6 pins are open through congestion: more search (--work, --variants) or more copper layers (--try-layers 2,4 routes each count and compares them)[^11]

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 86 | `/connections` |
| Routed | 82 | `/routed` |
| Open | 4 | `/open` |
| Routed, % | 95.35 | `/completion_pct` |
| Unconnected after zone refill | 4 | `/unconnected_after_refill` |
| Tracks added | 378 | `/tracks` |
| Vias added | 4 | `/vias` |
| Copper layers | 2 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | fast bends, dear vias, crowded source | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.1 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 3045751 | `/expansions` |
| Search expansions, all variants and passes | 58029151 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 80.461 | `/seconds` |

## What is open and why

Diagnosis of the open pins: an exact proof, past the board's fixed copper only, of whether a track or via can leave each pin at all. A pin it does not prove walled in is open through congestion: the copper routed so far is in its way.

|  | Value | JSON |
| --- | --- | --- |
| Pins of open connections | 6 | `/why_open/pins` |
| Walled in by fixed copper (no router connects them as placed) | 0 | `/why_open/walled_in` |
| Out only with a track or via smaller than the net class's | 0 | `/why_open/neck_down` |
| Walled in only by zone fills or copper graphics | 0 | `/why_open/soft_only` |
| Open through congestion | 6 | `/why_open/congested` |

Open connections by the router's cause (what failed when it last tried):

| Cause | Connections | What it means |
| --- | --- | --- |
| boxed\_in | 4 | When the router last tried, no track could leave a pad of the connection on its routing lattice: the copper round it (pads, keep-outs, and the tracks and vias routed by then) left no way out. Whether fixed copper alone does that is the diagnosis above: a pin it does not prove walled in is open through congestion, and moving routed copper can free it. |

Rows: `/stuck/unrouted_by_reason/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| JP1 | 4 |
| Q3 | 2 |
| P3 | 1 |
| R21 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each connection the router left open, in the router's order, with its last explanation and the nets in the way (those that blocked its search or, when none is named, those with the most routed copper round its two pads):

| Net | From | To | Distance between the pads, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| VCC | Q3 pad 3 | JP1 pad 1 | 2.577 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target); negotiated: boxed in (target); given up: boxed in on the routing lattice, even with routed copper negotiable | crowded by Net-(Q3-B) |
| /pic\_sockets/VCC\_PIC | JP1 pad 2 | R21 pad 2 | 4.526 | boxed\_in | boxed in; escapes/neck-down: boxed in; negotiated: boxed in; given up: boxed in on the routing lattice, even with routed copper negotiable |  |
| /pic\_sockets/VCC\_PIC | JP1 pad 2 | Q3 pad 1 | 6.567 | boxed\_in | boxed in; escapes/neck-down: boxed in; negotiated: boxed in; given up: boxed in on the routing lattice, even with routed copper negotiable | crowded by VCC, Net-(Q3-B) |
| /pic\_sockets/VCC\_PIC | P3 pad 11 | JP1 pad 2 | 34.145 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) | crowded by Net-(D8-A), Net-(D9-A), Net-(Q1-B), Net-(D12-A), Net-(D11-A) |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (4[^12] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it. The DRC names its own two ends for each gap, so they need not be the pads of the router's open connection.

Of these, 4[^13] are on nets the router left a connection of open (listed above) and 0[^14] on nets the router completed, cut off by the zone refill.

| Missing connection | From | To | X of From, mm | Y of From, mm | Net left open by the router |
| --- | --- | --- | --- | --- | --- |
| Missing connection in net VCC | Pad 1 of C1 \[VCC\] | Pad 1 of JP1 \[VCC\] | 110.49 | 78.867 | yes |
| Missing connection in net /pic\_sockets/VCC\_PIC | Pad 11 of P3 \[/pic\_sockets/VCC\_PIC\] | Pad 1 of Q3 \[/pic\_sockets/VCC\_PIC\] | 175.26 | 76.2 | yes |
| Missing connection in net /pic\_sockets/VCC\_PIC | Pad 1 of Q3 \[/pic\_sockets/VCC\_PIC\] | Pad 2 of R21 \[/pic\_sockets/VCC\_PIC\] | 142.24 | 97.79 | yes |
| Missing connection in net /pic\_sockets/VCC\_PIC | Pad 2 of R21 \[/pic\_sockets/VCC\_PIC\] | Pad 2 of JP1 \[/pic\_sockets/VCC\_PIC\] | 147.955 | 102.235 | yes |

Rows: `/drc/after/unconnected_items/<row>`

## Relax rescue

The first pass left connections open, so the board was routed again with relax, route, legalise: first under clearances lowered below the board's (the relaxed stages; their counts are not legal routes), then one round under the board's own rules that keeps only what is legal. A pass is kept only if its legal result beats the one before. Routed before the rescue: 82[^15]; a rescue result kept: no[^16].

| Pass | Routed under the board's rules | Kept | Seconds | Relaxed stages: clearance lowered by, mm, and routed |
| --- | --- | --- | --- | --- |
| warm | 82 | no | 21.38 | 0.1: 82 |
| fresh | 78 | no | 16.637 | 0.1: 82 |

Rows: `/rescue/passes/<row>`

## Zone refill

After the route the router refilled 1[^17] zones for the new copper; 7[^18] items were then unconnected.

Refill repair: the connections the refill cut off were routed again, with the first pass's work budget and what was left of the time limit; it is kept only if fewer items are unconnected after refilling once more.

|  | Value | JSON |
| --- | --- | --- |
| Connections tried | 7 | `/refill/repair/connections` |
| Routed | 3 | `/refill/repair/routed` |
| Tracks | 6 | `/refill/repair/tracks` |
| Vias | 2 | `/refill/repair/vias` |
| Search expansions | 3000224 | `/refill/repair/expansions` |
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
| Clearance, mm | 0 (not set) | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.25 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.9 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.5 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 (not set) | `/rules/minimums/hole_clearance_mm` |
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

Critical nets were looked for (from the component rules' roles, such as clocks, crystals and USB, and from named differential pairs) and the board has none, so the reference-return audit and the stitching of --si-rules had nothing to check.

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^20] (KiCad's violation names); zones refilled first: yes[^21]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 86 | 4 |

Rows: `/drc/before, /drc/after`

Unconnected items are counted as KiCad counts them, one per link missing from a net's copper. The router plans its own connections (with --soft-zones it does not count the old zone fills as joining pads), so the count before the route need not equal its connections.

Errors added of types routing can cause: 0[^22]; violations gone: 0[^23].

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
| --output | bench/results/run-report/pic\_programmer/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --no-kb | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-host | 0.0.0.0 |
| --view-port | 8813 |
| --json | bench/results/run-report/pic\_programmer/summary.json |
| --report | bench/results/run-report/pic\_programmer/report.html |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 77 | 7.54 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 82 | 9.531 | no |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 81 | 8.17 | no |  |
| first pass | 3 | fast bends, jittered order, escape plan | 82 | 8.856 | no |  |
| first pass | 4 | exact bends, jittered order | 71 | 6.685 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 82 | 6.762 | no |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 82 | 7.719 | no |  |
| first pass | 7 | fast bends, dear vias, crowded source | 82 | 7.596 | yes |  |
| rescue (warm) | 0 | exact bends, shortest first | 82 | 0.218 | yes | no |
| rescue (warm) | 1 | fast bends, shortest first, crowded source | 82 | 0.228 | no | no |
| rescue (warm) | 2 | fast bends, longest first (2x pitch on large boards) | 82 | 0.261 | no | no |
| rescue (warm) | 3 | fast bends, jittered order, escape plan | 82 | 0.413 | no | no |
| rescue (warm) | 4 | exact bends, jittered order | 82 | 0.303 | no | no |
| rescue (warm) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 82 | 0.178 | no | no |
| rescue (warm) | 6 | fast bends, cheap crossings, escape plan, crowded source | 82 | 0.271 | no | no |
| rescue (warm) | 7 | fast bends, dear vias, crowded source | 82 | 0.205 | no | no |
| rescue (fresh) | 0 | exact bends, shortest first | 78 | 0.346 | no | no |
| rescue (fresh) | 1 | fast bends, shortest first, crowded source | 78 | 0.36 | no | no |
| rescue (fresh) | 2 | fast bends, longest first (2x pitch on large boards) | 78 | 0.276 | no | no |
| rescue (fresh) | 3 | fast bends, jittered order, escape plan | 78 | 0.35 | yes | no |
| rescue (fresh) | 4 | exact bends, jittered order | 78 | 0.254 | no | no |
| rescue (fresh) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 78 | 0.363 | no | no |
| rescue (fresh) | 6 | fast bends, cheap crossings, escape plan, crowded source | 78 | 0.282 | no | no |
| rescue (fresh) | 7 | fast bends, dear vias, crowded source | 78 | 0.286 | no | no |

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
[^20]: `/drc/checker`
[^21]: `/drc/zones_refilled`
[^22]: `/drc/added_routing_errors`
[^23]: `/drc/removed`
