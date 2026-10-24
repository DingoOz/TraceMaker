# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.10.0[^2], 2026-10-24T01:49:02Z[^3]. Input build/demos/StickHub//unrouted.kicad\_pcb[^4], output bench/results/run-report/StickHub/routed.kicad\_pcb[^5].

> **7[^6] of 128[^7] connections are open; 94.53[^8] % are routed.**

> **After the zones are refilled for the new copper, 10[^9] items are unconnected: the board is not finished. See What is open and why.**

The route added 0[^10] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 128 | `/connections` |
| Routed | 121 | `/routed` |
| Open | 7 | `/open` |
| Routed, % | 94.53 | `/completion_pct` |
| Unconnected after zone refill | 10 | `/unconnected_after_refill` |
| Tracks added | 504 | `/tracks` |
| Vias added | 70 | `/vias` |
| Winning variant | fast bends, shortest first | `/variant_name` |
| Lattice pitch, mm | 0.05 | `/pitch_mm` |
| Search expansions, all variants | 16856330 | `/work_all_variants/total/expansions` |
| Wall time, s | 210.103 | `/seconds` |

## What is open and why

Open connections by cause:

| Cause | Connections | What it means |
| --- | --- | --- |
| ripped\_not\_rerouted | 6 | It was routed, then ripped up to make room for another net, and could not be routed again. |
| boxed\_in | 1 | Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it on the layers it can use. |

Rows: `/stuck/unrouted_by_reason/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U1 | 5 |
| D17 | 1 |
| D19 | 1 |
| D23 | 1 |
| J3 | 1 |
| J4 | 1 |
| J5 | 1 |
| J8 | 1 |
| R11 | 1 |
| R9 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each open connection, with the router's last explanation and the nets whose copper was in the way:

| Net | From | To | Length, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| /D+ | D23 pad 2 | U1 pad 30 | 6.034 | ripped\_not\_rerouted | ripped up by /D- and not routed again |  |
| /U7D+ | J8 pad 3 | U1 pad 38 | 6.445 | ripped\_not\_rerouted | ripped up by Net-(U1-VBUS\_SENSE) and not routed again |  |
| /U3D+ | J4 pad 3 | U1 pad 14 | 10.434 | ripped\_not\_rerouted | ripped up by /U3D- and not routed again |  |
| /U4D- | J5 pad 2 | U1 pad 11 | 13.589 | ripped\_not\_rerouted | ripped up by +1V8 and not routed again |  |
| Net-(D19-1) | D19 pad GA | R11 pad 1 | 5.766 | ripped\_not\_rerouted | ripped up by +3.3V and not routed again |  |
| +5V | J3 pad 1 | U1 pad 8 | 3.758 | ripped\_not\_rerouted | ripped up by +1V8 and not routed again |  |
| Net-(D17-1) | D17 pad GA | R9 pad 1 | 11.707 | boxed\_in | boxed in (target); escapes/neck-down: out of budget; negotiated: out of budget |  |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (10[^11] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it.

| Missing connection | From | To | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| Missing connection in net GND | Pad 1 of D4 \[GND\] | Pad 2 of C9 \[GND\] | 150.4 | 96.3 |
| Missing connection in net +5V | Pad 1 of J7 \[+5V\] | Pad 1 of J5 \[+5V\] | 148.15 | 96.25 |
| Missing connection in net +5V | Pad 1 of J5 \[+5V\] | Pad 8 of U1 \[+5V\] | 151.85 | 85.25 |
| Missing connection in net /U7D+ | Pad 3 of J8 \[/U7D+\] | Pad 38 of U1 \[/U7D+\] | 148.15 | 91.25 |
| Missing connection in net Net-(D19-1) | Pad GA of D19 \[Net-(D19-1)\] | Pad 1 of R11 \[Net-(D19-1)\] | 145.753 | 86.5 |
| Missing connection in net /U4D- | Pad 2 of J5 \[/U4D-\] | Pad 11 of U1 \[/U4D-\] | 151.85 | 84.25 |
| Missing connection in net /U3D+ | Pad 2 of D6 \[/U3D+\] | Pad 14 of U1 \[/U3D+\] | 150.4 | 90.2 |
| Missing connection in net /D+ | Pad 3 of J1 \[/D+\] | Pad 30 of U1 \[/D+\] | 148.73 | 114.775 |
| Missing connection in net Net-(D17-1) | Pad GA of D17 \[Net-(D17-1)\] | Pad 1 of R9 \[Net-(D17-1)\] | 147.753 | 83.7 |
| Missing connection in net +1V8 | Pad 1 of C11 \[+1V8\] | Pad 28 of U1 \[+1V8\] | 154.644 | 94.252 |

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
| Clearance, mm | 0.15 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.15 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.5 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.1 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.25 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0 | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 47 | 0.15 | 0.15 | 0.5 | 0.3 | 0.15 | 0.15 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

Critical nets: 18[^12] nets, 13[^13] vias, 48[^14] crossings of a reference-plane void.

| Net | Tier | Why critical | Vias | Length, mm | Unrouted | Void crossings | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- |
| /XI | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 3.443 | 0 | 1 | crosses 1 split of its reference plane |
| /XO | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 4.954 | 0 | 1 | crosses 1 split of its reference plane |
| /U2D+ | 2 | differential pair | 1 | 6.622 | 0 | 2 | runs over 2 voids of its reference plane (detour 0.00 mm) |
| /U6D- | 2 | differential pair | 1 | 5.29 | 0 | 2 | runs over 2 voids of its reference plane (detour 4.67 mm) |
| /U6D+ | 2 | differential pair | 1 | 4.856 | 0 | 3 | runs over 3 voids of its reference plane (detour 0.54 mm) |
| /U7D- | 2 | differential pair | 1 | 10.622 | 0 | 4 | crosses 3 splits of its reference plane |
| /U7D+ | 2 | differential pair | 0 | 1.452 | 1 | 1 | 1 connection not routed, runs over 1 void of its reference plane (detour 0.00 mm) |
| /U5D+ | 2 | differential pair | 1 | 10.269 | 0 | 5 | crosses 3 splits of its reference plane |
| /U4D+ | 2 | differential pair | 1 | 19.003 | 0 | 5 | crosses 3 splits of its reference plane |
| /U5D- | 2 | differential pair | 1 | 6.712 | 0 | 3 | crosses 2 splits of its reference plane |
| /U1D- | 2 | differential pair | 1 | 7.493 | 0 | 4 | crosses 1 split of its reference plane |
| /U3D- | 2 | differential pair | 1 | 16.834 | 0 | 5 | crosses 3 splits of its reference plane |
| /U2D- | 2 | differential pair | 1 | 5.199 | 0 | 2 | crosses 1 split of its reference plane |
| /U1D+ | 2 | differential pair | 1 | 6.221 | 0 | 1 | crosses 1 split of its reference plane |
| /U4D- | 2 | differential pair | 0 | 1.452 | 1 | 1 | 1 connection not routed, runs over 1 void of its reference plane (detour 0.00 mm) |
| /U3D+ | 2 | differential pair | 0 | 1.454 | 1 | 1 | 1 connection not routed, crosses 1 split of its reference plane |
| /D- | 2 | USB 2.0 (Full/High Speed) port (usb2 dm) | 1 | 16.827 | 0 | 5 | crosses 1 split of its reference plane |
| /D+ | 2 | USB 2.0 (Full/High Speed) port (usb2 dp) | 1 | 7.986 | 1 | 2 | 1 connection not routed, crosses 1 split of its reference plane |

Rows: `/critical_nets/nets/<row>`

## DRC before and after

Checked with tracemaker drc[^15] (KiCad's violation names); zones refilled first: yes[^16]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 0 | 0 |
| Warnings | 0 | 0 |
| Unconnected items | 128 | 10 |

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
| --output | bench/results/run-report/StickHub/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8816 |
| --json | bench/results/run-report/StickHub/summary.json |
| --report | bench/results/run-report/StickHub/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 37 | 94.983 |
| 1 | fast bends, shortest first | 121 | 20.768 |
| 2 | fast bends, longest first (2x pitch on large boards) | 101 | 178.585 |
| 3 | fast bends, jittered order, escape plan | 110 | 18.724 |
| 4 | exact bends, jittered order | 35 | 79.523 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 94 | 29.032 |
| 6 | fast bends, cheap crossings, escape plan | 113 | 134.458 |
| 7 | fast bends, dear vias | 120 | 14.707 |

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
