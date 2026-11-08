# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-08T22:45:06Z[^3]. Input build/demos/complex\_hierarchy//unrouted.kicad\_pcb[^4], output bench/results/run-report/complex\_hierarchy/routed.kicad\_pcb[^5].

Every connection was routed: 87[^6] of 87[^7].

After the zones are refilled for the new copper, 0[^8] items are unconnected.

The route added 0[^9] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 87 | `/connections` |
| Routed | 87 | `/routed` |
| Open | 0 | `/open` |
| Routed, % | 100 | `/completion_pct` |
| Unconnected after zone refill | 0 | `/unconnected_after_refill` |
| Tracks added | 188 | `/tracks` |
| Vias added | 0 | `/vias` |
| Copper layers | 2 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | fast bends, longest first (2x pitch on large boards) | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.1 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 195652 | `/expansions` |
| Search expansions, all variants and passes | 9665926 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 11.914 | `/seconds` |

## What is open and why

Nothing is open.

## Relax rescue

No rescue pass ran: the first pass left nothing open, or the rescue was turned off.

## Zone refill

After the route the router refilled 1[^10] zones for the new copper; 0[^11] items were then unconnected.

Thermal reliefs starved by the new copper: 0[^12].

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
| Clearance, mm | 0.2 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.203 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.889 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.508 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 (not set) | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0.01 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 47 | 0.3 | 0.4 | 1.651 | 0.6 | 0.4 | 0.35 |
| power | 5 | 0.3 | 0.6 | 1.651 | 0.6 | 0.4 | 0.35 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

Critical nets were looked for (from the component rules' roles, such as clocks, crystals and USB, and from named differential pairs) and the board has none, so the reference-return audit and the stitching of --si-rules had nothing to check.

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^13] (KiCad's violation names); zones refilled first: yes[^14]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 87 | 0 |

Rows: `/drc/before, /drc/after`

Unconnected items are counted as KiCad counts them, one per link missing from a net's copper. The router plans its own connections (with --soft-zones it does not count the old zone fills as joining pads), so the count before the route need not equal its connections.

Errors added of types routing can cause: 0[^15]; violations gone: 0[^16].

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
| --output | bench/results/run-report/complex\_hierarchy/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --no-kb | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-host | 0.0.0.0 |
| --view-port | 8810 |
| --json | bench/results/run-report/complex\_hierarchy/summary.json |
| --report | bench/results/run-report/complex\_hierarchy/report.html |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 87 | 4.523 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 87 | 1.655 | no |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 87 | 1.056 | yes |  |
| first pass | 3 | fast bends, jittered order, escape plan | 87 | 2.021 | no |  |
| first pass | 4 | exact bends, jittered order | 87 | 4.836 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 87 | 9.096 | no |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 87 | 1.762 | no |  |
| first pass | 7 | fast bends, dear vias, crowded source | 87 | 1.613 | no |  |

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
[^10]: `/refill/zones`
[^11]: `/refill/unconnected`
[^12]: `/refill/starved_thermals`
[^13]: `/drc/checker`
[^14]: `/drc/zones_refilled`
[^15]: `/drc/added_routing_errors`
[^16]: `/drc/removed`
