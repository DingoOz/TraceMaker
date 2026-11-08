# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.12.1[^2], 2026-11-08T22:48:19Z[^3]. Input build/demos/RoyalBlue54L-Feather//unrouted.kicad\_pcb[^4], output bench/results/run-report/RoyalBlue54L-Feather/routed.kicad\_pcb[^5].

> **19[^6] of 147[^7] connections are open; 87.07[^8] % are routed.**

> **After the zones are refilled for the new copper, 38[^9] items are unconnected: the board is not finished. See [What is open and why](#what-is-open-and-why).**

The route added 0[^10] DRC errors of types routing can cause.

> **The input board already has 38[^11] DRC errors before the route; they are not the route's, and [DRC before and after](#drc-before-and-after) lists where they are.**

Next: 37 pins are open through congestion: more search (--work, --variants) or more copper layers (--try-layers 2,4 routes each count and compares them)[^12]

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 147 | `/connections` |
| Routed | 128 | `/routed` |
| Open | 19 | `/open` |
| Routed, % | 87.07 | `/completion_pct` |
| Unconnected after zone refill | 38 | `/unconnected_after_refill` |
| Tracks added | 526 | `/tracks` |
| Vias added | 94 | `/vias` |
| Copper layers | 8 | `/copper_layers` |
| Kept result (variant of the portfolio, or of a rescue pass) | fast bends, shortest first, crowded source | `/variant_name` |
| Lattice pitch (the routing grid's step), mm | 0.05 | `/pitch_mm` |
| Search expansions of the kept result (the router's unit of work) | 3063153 | `/expansions` |
| Search expansions, all variants and passes | 68479872 | `/work_all_variants/total/expansions` |
| Wall time of the whole run, s | 204.195 | `/seconds` |

## What is open and why

Diagnosis of the open pins: an exact proof, past the board's fixed copper only, of whether a track or via can leave each pin at all. A pin it does not prove walled in is open through congestion: the copper routed so far is in its way.

|  | Value | JSON |
| --- | --- | --- |
| Pins of open connections | 37 | `/why_open/pins` |
| Walled in by fixed copper (no router connects them as placed) | 0 | `/why_open/walled_in` |
| Out only with a track or via smaller than the net class's | 0 | `/why_open/neck_down` |
| Walled in only by zone fills or copper graphics | 0 | `/why_open/soft_only` |
| Open through congestion | 37 | `/why_open/congested` |

Open connections by the router's cause (what failed when it last tried):

| Cause | Connections | What it means |
| --- | --- | --- |
| boxed\_in | 17 | When the router last tried, no track could leave a pad of the connection on its routing lattice: the copper round it (pads, keep-outs, and the tracks and vias routed by then) left no way out. Whether fixed copper alone does that is the diagnosis above: a pin it does not prove walled in is open through congestion, and moving routed copper can free it. |
| not\_attempted | 2 | No search was left for it: the budget (work or time) ended before the router tried it again, or, after the relax rescue, the last round, which keeps only the routes that are legal under the board's rules and does not search, left it open. The Why column says which. |

Rows: `/stuck/unrouted_by_reason/<row>`

The package analysis's coarse lattice found no way out for these pins either, but the exact proof finds one, so they are not counted as trapped:

| Part | Pads |
| --- | --- |
| U5 | 12, 13 |

Rows: `/stuck/escape_analysis/<row>/not_proved`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U1 | 10 |
| U5 | 6 |
| U6 | 5 |
| J1 | 3 |
| U2 | 3 |
| J2 | 2 |
| C14 | 1 |
| C5 | 1 |
| J3 | 1 |
| J8 | 1 |
| R2 | 1 |
| R3 | 1 |
| R8 | 1 |
| TP10 | 1 |
| U3 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each connection the router left open, in the router's order, with its last explanation and the nets in the way (those that blocked its search or, when none is named, those with the most routed copper round its two pads):

| Net | From | To | Distance between the pads, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| Net-(J8-R̅E̅S̅E̅T̅) | U5 pad 16 | TP10 pad 1 | 5.713 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/nRF52\_VDD, Net-(J8-SWDIO), Net-(J6-Pin\_4), /Debugger/D+, Net-(J8-SWCLK), /Debugger/D- |
| /Debugger/TRG\_R̅E̅S̅E̅T̅\_NRF52 | R8 pad 2 | U5 pad 34 | 7.286 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/nRF52\_VDD, /Debugger/TRG\_SWDCLK\_NRF52, Net-(U5-DEC4), Net-(U5-DEC1), Net-(JP1-A), VBUS |
| Net-(J8-R̅E̅S̅E̅T̅) | U5 pad 16 | J8 pad 3 | 8.735 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/D+, Net-(J8-SWCLK), Net-(J8-SWDIO), /Debugger/USB.CC2, /Debugger/USB.VBUS, /Debugger/nRF52\_VDD |
| /Debugger/SWD\_TRG.SWDIO | U1 pad J3 | U6 pad 11 | 10.42 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /nPM1300/NPM1300.SDA, /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /nPM1300/NPM1300.SCL, /Debugger/SWD\_TRG.SWDCLK, /Connectors/ANALOG.A1, /Connectors/SPI.MOSI |
| /Connectors/UART.TX | U1 pad B2 | U6 pad 13 | 11.385 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by Net-(J6-Pin\_4), /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /Connectors/UART.RX, /nPM1300/NPM1300.SDA, /nPM1300/NPM1300.SCL, /Connectors/DIG.D11 |
| /Connectors/ANALOG.A5 | U1 pad B5 | J1 pad 10 | 16.289 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Connectors/UART.RX, /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /Connectors/ANALOG.A4, Net-(J6-Pin\_4), /Connectors/SPI.MOSI, /Connectors/DIG.D11 |
| /Connectors/DIG.D12 | U1 pad J5 | J2 pad 5 | 17.874 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Connectors/DIG.D11, /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /Connectors/UART.RX, Net-(J6-Pin\_4), /Connectors/DIG.D10, /nPM1300/NPM1300.SCL |
| /Debugger/UART\_RX\_NRF52 | U6 pad 3 | U5 pad 36 | 17.534 | boxed\_in | boxed in; escapes/neck-down: boxed in (target) | crowded by /Debugger/nRF52\_VDD, /Debugger/TRG\_SWDCLK\_NRF52, Net-(JP1-A), Net-(J6-Pin\_4), /Debugger/TRG\_R̅E̅S̅E̅T̅\_NRF52, VSYS |
| /Debugger/UART\_TX\_NRF52 | U6 pad 4 | U5 pad 35 | 17.734 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/nRF52\_VDD, /Debugger/TRG\_SWDCLK\_NRF52, Net-(JP1-A), /Debugger/TRG\_R̅E̅S̅E̅T̅\_NRF52, VSYS, VBUS |
| /Debugger/USB.VBUS | C5 pad 1 | U3 pad 5 | 18.348 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/USB.CC2, Net-(J8-SWCLK), Net-(J6-Pin\_4), /Debugger/D-, /Debugger/D+, VBUS |
| /Debugger/TRG\_SWDIO\_NRF52 | U6 pad 6 | U5 pad 33 | 18.196 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/nRF52\_VDD, /Debugger/TRG\_SWDCLK\_NRF52, Net-(JP1-A), /Debugger/TRG\_R̅E̅S̅E̅T̅\_NRF52, VSYS, Net-(J8-SWCLK) |
| /Connectors/DIG.D13 | U1 pad J6 | J2 pad 4 | 19.008 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Connectors/DIG.D11, Net-(J6-Pin\_4), /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /Connectors/UART.RX, /nPM1300/NPM1300.SCL, /nPM1300/NPM1300.SDA |
| /nPM1300/NPM1300.INT | U1 pad K2 | U2 pad 7 | 19.307 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) | crowded by /Debugger/SWD\_TRG.SWDCLK, /nPM1300/NPM1300.SCL, /Connectors/ANALOG.A1, /nPM1300/NPM1300.SDA, /Connectors/SPI.MOSI, /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅ |
| /Connectors/ANALOG.A0 | U1 pad E9 | J1 pad 5 | 21.101 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /Connectors/ANALOG.A1, /Debugger/nRF52\_VDD, /Connectors/ANALOG.A2, /Connectors/ANALOG.A3, Net-(JP1-A) |
| /Debugger/USB.CC1 | U2 pad 23 | J3 pad A5 | 20.996 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) | crowded by /Debugger/D-, /Debugger/USB.CC2, Net-(J6-Pin\_4), /Debugger/USB.VBUS, /Debugger/D+, VBUS |
| /Connectors/SHPHLD | U2 pad 15 | J1 pad 16 | 24.274 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /nPM1300/NPM1300.SCL, /nPM1300/NPM1300.SDA, /Connectors/ANALOG.A1, /Connectors/ANALOG.A4, /Connectors/UART.RX |
| Net-(J6-Pin\_2) | U1 pad D9 | C14 pad 1 | 35.136 | boxed\_in | boxed in; escapes/neck-down: boxed in | crowded by /Debugger/nRF52\_VDD, Net-(J6-Pin\_4), /Debugger/SWD\_TRG.R̅E̅S̅E̅T̅, /Debugger/TRG\_SWDCLK\_NRF52, /Connectors/ANALOG.A1, /Debugger/D+ |
| /Connectors/I2C.SCL | U1 pad G8 | R2 pad 2 | 36.019 | not\_attempted | (none: not tried again before the budget ended) | crowded by /Debugger/nRF52\_VDD, Net-(J6-Pin\_4), /Debugger/USB.CC2, /Debugger/D+, /nPM1300/NPM1300.SCL, /Connectors/UART.RX |
| /Connectors/I2C.SDA | U1 pad B3 | R3 pad 2 | 39.105 | not\_attempted | (none: not tried again before the budget ended) | crowded by Net-(J6-Pin\_4), /Debugger/USB.CC2, /nPM1300/NPM1300.SCL, /Connectors/DIG.D11, /Connectors/UART.RX, VBUS |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (38[^13] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it. The DRC names its own two ends for each gap, so they need not be the pads of the router's open connection.

Of these, 19[^14] are on nets the router left a connection of open (listed above) and 19[^15] on nets the router completed, cut off by the zone refill.

| Missing connection | From | To | X of From, mm | Y of From, mm | Net left open by the router |
| --- | --- | --- | --- | --- | --- |
| Missing connection in net GND | Pad 2 of C25 \[GND\] | Pad 2 of J4 \[GND\] | 154.19 | 101.81 | no |
| Missing connection in net GND | Pad 2 of J4 \[GND\] | Pad A1 of U1 \[GND\] | 135.88 | 101.308 | no |
| Missing connection in net GND | Pad A1 of U1 \[GND\] | Pad A9 of U1 \[GND\] | 168.92 | 100.93 | no |
| Missing connection in net GND | Pad A9 of U1 \[GND\] | Pad H8 of U1 \[GND\] | 161.72 | 100.93 | no |
| Missing connection in net GND | Pad H8 of U1 \[GND\] | Pad 2 of C10 \[GND\] | 162.62 | 107.23 | no |
| Missing connection in net GND | Pad 2 of C10 \[GND\] | Pad 2 of U2 \[GND\] | 140.44 | 108.24 | no |
| Missing connection in net GND | Pad 2 of U2 \[GND\] | Pad 6 of U2 \[GND\] | 146.33 | 106.91 | no |
| Missing connection in net GND | Pad 6 of U2 \[GND\] | Pad 18 of U2 \[GND\] | 148.33 | 106.91 | no |
| Missing connection in net GND | Pad 18 of U2 \[GND\] | Pad 28 of U2 \[GND\] | 148.83 | 102.01 | no |
| Missing connection in net GND | Pad 28 of U2 \[GND\] | Pad 2 of C19 \[GND\] | 145.13 | 104.21 | no |
| Missing connection in net GND | Pad 2 of C19 \[GND\] | Pad A1 of J3 \[GND\] | 140.44 | 111.01 | no |
| Missing connection in net GND | Pad A1 of J3 \[GND\] | Pad 4 of Y2 \[GND\] | 125.405 | 101.73 | no |
| Missing connection in net GND | Pad 4 of Y2 \[GND\] | Pad 2 of C22 \[GND\] | 132.24 | 109.5 | no |
| Missing connection in net GND | Pad 2 of C22 \[GND\] | Pad 1 of SW1 \[GND\] | 132.3 | 106.78 | no |
| Missing connection in net GND | Pad 1 of SW1 \[GND\] | Pad 2 of C15 \[GND\] | 157.835 | 111.605 | no |
| Missing connection in net GND | Pad 2 of C15 \[GND\] | Pad 3 of U3 \[GND\] | 139.55 | 111.69 | no |
| Missing connection in net GND | Pad 3 of U3 \[GND\] | Pad 2 of C7 \[GND\] | 127.898 | 105.473 | no |
| Missing connection in net GND | Pad 2 of C7 \[GND\] | Pad 2 of C17 \[GND\] | 140.44 | 105.46 | no |
| Missing connection in net GND | Pad 2 of C17 \[GND\] | Pad 2 of C12 \[GND\] | 134.91 | 104.4 | no |
| Missing connection in net /Connectors/UART.TX | Pad B2 of U1 \[/Connectors/UART.TX\] | Pad 13 of U6 \[/Connectors/UART.TX\] | 168.02 | 101.83 | yes |
| Missing connection in net /Connectors/I2C.SDA | Pad B3 of U1 \[/Connectors/I2C.SDA\] | Pad 2 of R3 \[/Connectors/I2C.SDA\] | 167.12 | 101.83 | yes |
| Missing connection in net /Connectors/ANALOG.A5 | Pad B5 of U1 \[/Connectors/ANALOG.A5\] | Pad 10 of J1 \[/Connectors/ANALOG.A5\] | 165.32 | 101.83 | yes |
| Missing connection in net Net-(J6-Pin\_2) | Pad D9 of U1 \[Net-(J6-Pin\_2)\] | Pad 2 of J6 \[Net-(J6-Pin\_2)\] | 161.72 | 103.63 | yes |
| Missing connection in net /Connectors/ANALOG.A0 | Pad E9 of U1 \[/Connectors/ANALOG.A0\] | Pad 5 of J1 \[/Connectors/ANALOG.A0\] | 161.72 | 104.53 | yes |
| Missing connection in net /Connectors/I2C.SCL | Pad G8 of U1 \[/Connectors/I2C.SCL\] | Pad 2 of R2 \[/Connectors/I2C.SCL\] | 162.62 | 106.33 | yes |
| Missing connection in net /Debugger/SWD\_TRG.SWDIO | Pad J3 of U1 \[/Debugger/SWD\_TRG.SWDIO\] | Pad 11 of U6 \[/Debugger/SWD\_TRG.SWDIO\] | 167.12 | 108.13 | yes |
| Missing connection in net /Connectors/DIG.D12 | Pad J5 of U1 \[/Connectors/DIG.D12\] | Pad 5 of J2 \[/Connectors/DIG.D12\] | 165.32 | 108.13 | yes |
| Missing connection in net /Connectors/DIG.D13 | Pad J6 of U1 \[/Connectors/DIG.D13\] | Pad 4 of J2 \[/Connectors/DIG.D13\] | 164.42 | 108.13 | yes |
| Missing connection in net /nPM1300/NPM1300.INT | Pad K2 of U1 \[/nPM1300/NPM1300.INT\] | Pad 7 of U2 \[/nPM1300/NPM1300.INT\] | 168.02 | 109.03 | yes |
| Missing connection in net /Connectors/SHPHLD | Pad 15 of U2 \[/Connectors/SHPHLD\] | Pad 16 of J1 \[/Connectors/SHPHLD\] | 150.03 | 103.21 | yes |
| Missing connection in net /Debugger/USB.VBUS | Pad 21 of U2 \[/Debugger/USB.VBUS\] | Pad A4 of J3 \[/Debugger/USB.VBUS\] | 147.33 | 102.01 | yes |
| Missing connection in net /Debugger/USB.CC1 | Pad 23 of U2 \[/Debugger/USB.CC1\] | Pad A5 of J3 \[/Debugger/USB.CC1\] | 146.33 | 102.01 | yes |
| Missing connection in net /Debugger/TRG\_R̅E̅S̅E̅T̅\_NRF52 | Pad 2 of R8 \[/Debugger/TRG\_R̅E̅S̅E̅T̅\_NRF52\] | Pad 34 of U5 \[/Debugger/TRG\_R̅E̅S̅E̅T̅\_NRF52\] | 143.11 | 108.03 | yes |
| Missing connection in net /Debugger/UART\_RX\_NRF52 | Pad 3 of U6 \[/Debugger/UART\_RX\_NRF52\] | Pad 36 of U5 \[/Debugger/UART\_RX\_NRF52\] | 153.405 | 104.17 | yes |
| Missing connection in net /Debugger/UART\_TX\_NRF52 | Pad 4 of U6 \[/Debugger/UART\_TX\_NRF52\] | Pad 35 of U5 \[/Debugger/UART\_TX\_NRF52\] | 153.405 | 104.67 | yes |
| Missing connection in net /Debugger/TRG\_SWDIO\_NRF52 | Pad 6 of U6 \[/Debugger/TRG\_SWDIO\_NRF52\] | Pad 33 of U5 \[/Debugger/TRG\_SWDIO\_NRF52\] | 153.405 | 105.67 | yes |
| Missing connection in net Net-(J8-R̅E̅S̅E̅T̅) | Pad 16 of U5 \[Net-(J8-R̅E̅S̅E̅T̅)\] | Pad 1 of TP10 \[Net-(J8-R̅E̅S̅E̅T̅)\] | 136.68 | 105.693 | yes |
| Missing connection in net Net-(J8-R̅E̅S̅E̅T̅) | Pad 1 of TP10 \[Net-(J8-R̅E̅S̅E̅T̅)\] | Pad 3 of J8 \[Net-(J8-R̅E̅S̅E̅T̅)\] | 136.7 | 99.98 | yes |

Rows: `/drc/after/unconnected_items/<row>`

## Relax rescue

The first pass left connections open, so the board was routed again with relax, route, legalise: first under clearances lowered below the board's (the relaxed stages; their counts are not legal routes), then one round under the board's own rules that keeps only what is legal. A pass is kept only if its legal result beats the one before. Routed before the rescue: 128[^16]; a rescue result kept: no[^17].

| Pass | Routed under the board's rules | Kept | Seconds | Relaxed stages: clearance lowered by, mm, and routed |
| --- | --- | --- | --- | --- |
| warm | 128 | no | 43.813 | 0.06: 130 |
| fresh | 105 | no | 50.946 | 0.06: 126 |

Rows: `/rescue/passes/<row>`

## Zone refill

After the route the router refilled 4[^18] zones for the new copper; 38[^19] items were then unconnected.

Refill repair: the connections the refill cut off were routed again, with the first pass's work budget and what was left of the time limit; it is kept only if fewer items are unconnected after refilling once more.

|  | Value | JSON |
| --- | --- | --- |
| Connections tried | 38 | `/refill/repair/connections` |
| Routed | 0 | `/refill/repair/routed` |
| Tracks | 0 | `/refill/repair/tracks` |
| Vias | 0 | `/refill/repair/vias` |
| Search expansions | 3244436 | `/refill/repair/expansions` |
| Unconnected after refilling again | 38 | `/refill/repair/unconnected` |
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
| Clearance, mm | 0.076 | `/rules/minimums/clearance_mm` |
| Track width, mm | 0.076 | `/rules/minimums/track_width_mm` |
| Via diameter, mm | 0.35 | `/rules/minimums/via_diameter_mm` |
| Via annular width, mm | 0.05 | `/rules/minimums/via_annular_width_mm` |
| Drill, mm | 0.2 | `/rules/minimums/drill_mm` |
| Hole clearance, mm | 0.127 | `/rules/minimums/hole_clearance_mm` |
| Hole to hole, mm | 0.25 | `/rules/minimums/hole_to_hole_mm` |
| Copper to edge, mm | 0.127 | `/rules/minimums/copper_edge_clearance_mm` |
| Blind and buried vias | no | `/rules/minimums/blind_buried_vias` |
| Microvias | no | `/rules/minimums/microvias` |

Net classes:

| Class | Nets | Clearance, mm | Track, mm | Via, mm | Drill, mm | Pair width, mm | Pair gap, mm |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Default | 93 | 0.15 | 0.25 | 0.8 | 0.4 | 0.2 | 0.25 |
| USB\_DIFF | 2 | 0.203 | 0.125 | 0.8 | 0.4 | 0.125 | 0.203 |

Rows: `/rules/net_classes/<row>`

No custom rules.

## Signal-integrity audit

Critical nets: 6[^21] nets, 2[^22] vias, 16[^23] crossings of a reference-plane void.

Layer changes without a return via within reach: 0[^24] (before the stitching pass of --si-rules: 0[^25]).

Void crossings counts the voids of the reference plane a net's tracks run over; a split is a void that cuts the plane in two, counted on its own and named first in the warnings. The return detour is the extra way the return current takes round the voids, so it can be longer than the net's own track. The warnings name each void whose detour is over 2[^26] mm, so the listed detours need not add up to the total. New track is the length of the net's new copper, routed connections only.

| Net | Tier | Why critical | Vias | New track, mm | Open connections | Void crossings | Splits | Return detour, mm | Unstitched layer changes | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| Net-(U1-P1.01/XL2) | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 5.175 | 0 | 1 | 0 | 11.361 | 0 | runs over 1 void of its reference plane (detour 11.36 mm), detour of 11.36 mm round a void on F.Cu at (160.900, 101.970) mm |
| Net-(U1-P1.00/XL1) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 16.951 | 0 | 3 | 0 | 10.744 | 0 | runs over 3 voids of its reference plane (detour 10.74 mm), detour of 3.20 mm round a void on F.Cu at (168.996, 100.246) mm, detour of 5.72 mm round a void on F.Cu at (166.366, 98.230) mm |
| Net-(U5-XC1) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 5.147 | 0 | 1 | 0 | 3.476 | 0 | runs over 1 void of its reference plane (detour 3.48 mm), detour of 3.48 mm round a void on F.Cu at (133.710, 109.130) mm |
| Net-(U5-XC2) | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 3.959 | 0 | 1 | 0 | 4.537 | 0 | runs over 1 void of its reference plane (detour 4.54 mm), detour of 4.54 mm round a void on F.Cu at (134.110, 109.530) mm |
| /Debugger/D+ | 2 | USB 2.0 (Full/High Speed) port (usb2 dp) | 1 | 20.563 | 0 | 5 | 0 | 17.75 | 0 | runs over 5 voids of its reference plane (detour 17.75 mm), detour of 3.90 mm round a void on F.Cu at (127.680, 105.950) mm, detour of 9.40 mm round a void on F.Cu at (133.759, 103.750) mm, detour of 3.63 mm round a void on F.Cu at (137.174, 104.424) mm |
| /Debugger/D- | 2 | USB 2.0 (Full/High Speed) port (usb2 dm) | 1 | 24.277 | 0 | 5 | 0 | 25.447 | 0 | runs over 5 voids of its reference plane (detour 25.45 mm), detour of 9.65 mm round a void on F.Cu at (128.550, 104.526) mm, detour of 10.53 mm round a void on F.Cu at (134.159, 103.350) mm, detour of 4.25 mm round a void on F.Cu at (137.726, 104.426) mm |

Rows: `/critical_nets/nets/<row>`

Not checked, because not asked for: differential pairs' coupling, gap and skew (--diff-pairs); buses (--bus or --buses); the return paths of nets of your choice (--return-paths).

## DRC before and after

Checked with tracemaker drc[^27] (KiCad's violation names); zones refilled first: yes[^28]. The comparison counts violations by type; sign off with kicad-cli pcb drc. Thermal reliefs (starved\_thermal) are not checked here; see Zone refill.

|  | Before | After |
| --- | --- | --- |
| Errors | 38 | 38 |
| Warnings | 0 | 0 |
| Unconnected items | 146 | 38 |

Rows: `/drc/before, /drc/after`

Unconnected items are counted as KiCad counts them, one per link missing from a net's copper. The router plans its own connections (with --soft-zones it does not count the old zone fills as joining pads), so the count before the route need not equal its connections.

Errors added of types routing can cause: 0[^29]; violations gone: 0[^30].

| Type | Severity | Routing can cause it | Before | After | Added |
| --- | --- | --- | --- | --- | --- |
| clearance | error | yes | 10 | 10 | 0 |
| copper\_edge\_clearance | error | yes | 28 | 28 | 0 |

Rows: `/drc/types/<row>`

Errors already in the input board, before the route, with the first item of each and where it is:

| Type | What | Item | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| clearance |  | Pad A5 of J3 \[/Debugger/USB.CC1\] | 125.405 | 103.73 |
| clearance |  | Pad A6 of J3 \[/Debugger/D+\] | 125.405 | 104.73 |
| clearance |  | Pad A6 of J3 \[/Debugger/D+\] | 125.405 | 104.73 |
| clearance |  | Pad A7 of J3 \[/Debugger/D-\] | 125.405 | 105.23 |
| clearance |  | Pad A8 of J3 \[unconnected-(J3-SBU1-PadA8)\] | 125.405 | 106.23 |
| clearance |  | Pad 1 of U3 \[unconnected-(U3-IO1-Pad1)\] | 127.898 | 104.473 |
| clearance |  | Pad 2 of U3 \[/Debugger/D-\] | 127.898 | 104.973 |
| clearance |  | Pad 11 of U5 \[Net-(U5-DECUSB)\] | 138.68 | 105.693 |
| clearance |  | Pad 12 of U5 \[/Debugger/D-\] | 138.28 | 105.693 |
| clearance |  | Pad 13 of U5 \[/Debugger/D+\] | 137.88 | 105.693 |
| copper\_edge\_clearance |  | Pad 1 of J2 \[+BATT\] | 143.23 | 93.55 |
| copper\_edge\_clearance |  | Pad 2 of J2 \[/Connectors/ENABLE\] | 145.77 | 93.55 |
| copper\_edge\_clearance |  | Pad 3 of J2 \[VSYS\] | 148.31 | 93.55 |
| copper\_edge\_clearance |  | Pad 4 of J2 \[/Connectors/DIG.D13\] | 150.85 | 93.55 |
| copper\_edge\_clearance |  | Pad 5 of J2 \[/Connectors/DIG.D12\] | 153.39 | 93.55 |
| copper\_edge\_clearance |  | Pad 6 of J2 \[/Connectors/DIG.D11\] | 155.93 | 93.55 |
| copper\_edge\_clearance |  | Pad 7 of J2 \[/Connectors/DIG.D10\] | 158.47 | 93.55 |
| copper\_edge\_clearance |  | Pad 8 of J2 \[/Connectors/DIG.D9\] | 161.01 | 93.55 |
| copper\_edge\_clearance |  | Pad 9 of J2 \[/Connectors/DIG.D6\] | 163.55 | 93.55 |
| copper\_edge\_clearance |  | Pad 10 of J2 \[/Connectors/DIG.D5\] | 166.09 | 93.55 |
| copper\_edge\_clearance |  | Pad 11 of J2 \[/Connectors/I2C.SCL\] | 168.63 | 93.55 |
| copper\_edge\_clearance |  | Pad 12 of J2 \[/Connectors/I2C.SDA\] | 171.17 | 93.55 |
| copper\_edge\_clearance |  | Pad 1 of J1 \[/Debugger/SWD\_TRG.R̅E̅S̅E̅T̅\] | 133.07 | 116.41 |
| copper\_edge\_clearance |  | Pad 2 of J1 \[VDD\] | 135.61 | 116.41 |
| copper\_edge\_clearance |  | Pad 3 of J1 \[unconnected-(J1-Pin\_3-Pad3)\] | 138.15 | 116.41 |
| copper\_edge\_clearance |  | Pad 4 of J1 \[GND\] | 140.69 | 116.41 |
| copper\_edge\_clearance |  | Pad 5 of J1 \[/Connectors/ANALOG.A0\] | 143.23 | 116.41 |
| copper\_edge\_clearance |  | Pad 6 of J1 \[/Connectors/ANALOG.A1\] | 145.77 | 116.41 |
| copper\_edge\_clearance |  | Pad 7 of J1 \[/Connectors/ANALOG.A2\] | 148.31 | 116.41 |
| copper\_edge\_clearance |  | Pad 8 of J1 \[/Connectors/ANALOG.A3\] | 150.85 | 116.41 |
| copper\_edge\_clearance |  | Pad 9 of J1 \[/Connectors/ANALOG.A4\] | 153.39 | 116.41 |
| copper\_edge\_clearance |  | Pad 10 of J1 \[/Connectors/ANALOG.A5\] | 155.93 | 116.41 |
| copper\_edge\_clearance |  | Pad 11 of J1 \[/Connectors/SPI.SCK\] | 158.47 | 116.41 |
| copper\_edge\_clearance |  | Pad 12 of J1 \[/Connectors/SPI.MOSI\] | 161.01 | 116.41 |
| copper\_edge\_clearance |  | Pad 13 of J1 \[/Connectors/SPI.MISO\] | 163.55 | 116.41 |
| copper\_edge\_clearance |  | Pad 14 of J1 \[/Connectors/UART.RX\] | 166.09 | 116.41 |
| copper\_edge\_clearance |  | Pad 15 of J1 \[/Connectors/UART.TX\] | 168.63 | 116.41 |
| copper\_edge\_clearance |  | Pad 16 of J1 \[/Connectors/SHPHLD\] | 171.17 | 116.41 |

Rows: `/drc/before/error_items/<row>`

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
| --output | bench/results/run-report/RoyalBlue54L-Feather/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --no-kb | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-host | 0.0.0.0 |
| --view-port | 8815 |
| --json | bench/results/run-report/RoyalBlue54L-Feather/summary.json |
| --report | bench/results/run-report/RoyalBlue54L-Feather/report.html |

Rows: `/run/options/<row>`

Portfolio variants, pass by pass (the first pass, then each rescue pass); the best of a pass is kept only if it beats the result before:

| Pass | Variant | Name | Routed | Seconds | Best of its pass | Pass kept |
| --- | --- | --- | --- | --- | --- | --- |
| first pass | 0 | exact bends, shortest first | 81 | 10.464 | no |  |
| first pass | 1 | fast bends, shortest first, crowded source | 128 | 47.338 | yes |  |
| first pass | 2 | fast bends, longest first (2x pitch on large boards) | 26 | 21.089 | no |  |
| first pass | 3 | fast bends, jittered order, escape plan | 104 | 22.51 | no |  |
| first pass | 4 | exact bends, jittered order | 81 | 11.18 | no |  |
| first pass | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 110 | 33.18 | no |  |
| first pass | 6 | fast bends, cheap crossings, escape plan, crowded source | 125 | 45.565 | no |  |
| first pass | 7 | fast bends, dear vias, crowded source | 126 | 42.996 | no |  |
| rescue (warm) | 0 | exact bends, shortest first | 127 | 2.199 | no | no |
| rescue (warm) | 1 | fast bends, shortest first, crowded source | 127 | 2.146 | no | no |
| rescue (warm) | 2 | fast bends, longest first (2x pitch on large boards) | 127 | 1.895 | no | no |
| rescue (warm) | 3 | fast bends, jittered order, escape plan | 128 | 2.153 | yes | no |
| rescue (warm) | 4 | exact bends, jittered order | 127 | 1.598 | no | no |
| rescue (warm) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 127 | 2.165 | no | no |
| rescue (warm) | 6 | fast bends, cheap crossings, escape plan, crowded source | 127 | 1.831 | no | no |
| rescue (warm) | 7 | fast bends, dear vias, crowded source | 127 | 2.147 | no | no |
| rescue (fresh) | 0 | exact bends, shortest first | 105 | 3.604 | no | no |
| rescue (fresh) | 1 | fast bends, shortest first, crowded source | 105 | 3.625 | no | no |
| rescue (fresh) | 2 | fast bends, longest first (2x pitch on large boards) | 105 | 3.606 | yes | no |
| rescue (fresh) | 3 | fast bends, jittered order, escape plan | 104 | 3.635 | no | no |
| rescue (fresh) | 4 | exact bends, jittered order | 105 | 3.622 | no | no |
| rescue (fresh) | 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source | 105 | 3.444 | no | no |
| rescue (fresh) | 6 | fast bends, cheap crossings, escape plan, crowded source | 105 | 3.722 | no | no |
| rescue (fresh) | 7 | fast bends, dear vias, crowded source | 105 | 3.697 | no | no |

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
[^11]: `/drc/before/errors`
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
