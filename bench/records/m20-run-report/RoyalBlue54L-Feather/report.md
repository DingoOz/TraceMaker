# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.11.0[^2], 2026-10-27T22:56:45Z[^3]. Input build/demos/RoyalBlue54L-Feather/unrouted.kicad\_pcb[^4], output build/scratch/d3/RoyalBlue54L-Feather/routed.kicad\_pcb[^5].

> **45[^6] of 147[^7] connections are open; 69.39[^8] % are routed.**

> **After the zones are refilled for the new copper, 61[^9] items are unconnected: the board is not finished. See What is open and why.**

The route added 0[^10] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 147 | `/connections` |
| Routed | 102 | `/routed` |
| Open | 45 | `/open` |
| Routed, % | 69.39 | `/completion_pct` |
| Unconnected after zone refill | 61 | `/unconnected_after_refill` |
| Tracks added | 331 | `/tracks` |
| Vias added | 57 | `/vias` |
| Winning variant | fast bends, dear vias | `/variant_name` |
| Lattice pitch, mm | 0.05 | `/pitch_mm` |
| Search expansions, all variants | 25017730 | `/work_all_variants/total/expansions` |
| Wall time, s | 71.651 | `/seconds` |

## What is open and why

Open connections by cause:

| Cause | Connections | What it means |
| --- | --- | --- |
| not\_attempted | 37 | The run ended (work or time) before the router tried it again. |
| boxed\_in | 8 | Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it on the layers it can use. |

Rows: `/stuck/unrouted_by_reason/<row>`

> **Some pins cannot leave their package at all under the board's rules; no amount of routing effort connects them.**

| Part | Pitch, mm | Pins that cannot escape | Pads | Why | What would help |
| --- | --- | --- | --- | --- | --- |
| U5 | 0.4 | 2 | 12, 13 | no channel at 0.125 mm and no via site within reach |  |

Rows: `/stuck/escape_analysis/<row>/dead/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U1 | 25 |
| U5 | 11 |
| U6 | 11 |
| J1 | 10 |
| U2 | 8 |
| J2 | 7 |
| R8 | 3 |
| J3 | 2 |
| J8 | 2 |
| C13 | 1 |
| C14 | 1 |
| C22 | 1 |
| C5 | 1 |
| D3 | 1 |
| R2 | 1 |
| R3 | 1 |

Rows: `/stuck/unrouted_by_part/<row>`

Each open connection, with the router's last explanation and the nets whose copper was in the way:

| Net | From | To | Length, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| Net-(U5-DEC4) | C22 pad 1 | U5 pad 26 | 1.95 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) |  |
| Net-(D3-GK) | D3 pad 3 | U2 pad 26 | 2.852 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) |  |
| Net-(U5-DEC4) | U5 pad 26 | U5 pad 38 | 4.102 | boxed\_in | boxed in; escapes/neck-down: boxed in |  |
| Net-(J8-~{RESET}) | U5 pad 16 | TP10 pad 1 | 5.713 | boxed\_in | boxed in; escapes/neck-down: boxed in |  |
| Net-(J8-SWDIO) | U5 pad 19 | J8 pad 2 | 6.259 | boxed\_in | boxed in; escapes/neck-down: boxed in |  |
| /Debugger/TRG\_~{RESET}\_NRF52 | R8 pad 2 | U5 pad 34 | 7.286 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) |  |
| Net-(J8-~{RESET}) | U5 pad 16 | J8 pad 3 | 8.735 | boxed\_in | boxed in; escapes/neck-down: boxed in |  |
| /Debugger/SWD\_TRG.SWDIO | U1 pad J3 | U6 pad 11 | 10.42 | boxed\_in | boxed in (target); escapes/neck-down: out of budget |  |
| /Debugger/SWD\_TRG.SWDCLK | U1 pad K3 | U6 pad 10 | 10.521 | not\_attempted |  |  |
| /Debugger/TRG\_~{RESET}\_NRF52 | R8 pad 2 | U6 pad 5 | 10.685 | not\_attempted |  |  |
| /Connectors/UART.RX | U1 pad A3 | U6 pad 14 | 10.631 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U6 pad 2 | R8 pad 1 | 10.823 | not\_attempted |  |  |
| /Connectors/DIG.D11 | U1 pad J4 | SW2 pad 1 | 11.334 | not\_attempted |  |  |
| /Connectors/UART.TX | U1 pad B2 | U6 pad 13 | 11.385 | not\_attempted |  |  |
| /Connectors/ENABLE | U2 pad 8 | J2 pad 2 | 12.603 | not\_attempted |  |  |
| /Connectors/I2C.SCL | U1 pad G8 | J2 pad 11 | 12.985 | not\_attempted |  |  |
| /Connectors/UART.TX | U1 pad B2 | J1 pad 15 | 13.324 | not\_attempted |  |  |
| /Connectors/ANALOG.A3 | U1 pad G9 | J1 pad 8 | 13.799 | not\_attempted |  |  |
| /Connectors/UART.RX | U6 pad 14 | J1 pad 14 | 14.25 | not\_attempted |  |  |
| /Connectors/DIG.D6 | U1 pad K4 | J2 pad 9 | 14.459 | not\_attempted |  |  |
| /Connectors/DIG.D9 | U1 pad K5 | J2 pad 8 | 14.849 | not\_attempted |  |  |
| /Connectors/DIG.D10 | U1 pad K6 | J2 pad 7 | 15.405 | not\_attempted |  |  |
| /Connectors/ANALOG.A2 | U1 pad F9 | J1 pad 7 | 16.355 | not\_attempted |  |  |
| /Connectors/ANALOG.A5 | U1 pad B5 | J1 pad 10 | 16.289 | not\_attempted |  |  |
| /Connectors/DIG.D12 | U1 pad J5 | J2 pad 5 | 17.874 | not\_attempted |  |  |
| /Debugger/UART\_RX\_NRF52 | U6 pad 3 | U5 pad 36 | 17.534 | not\_attempted |  |  |
| /Debugger/UART\_TX\_NRF52 | U6 pad 4 | U5 pad 35 | 17.734 | not\_attempted |  |  |
| /Connectors/ANALOG.A4 | U1 pad B4 | J1 pad 9 | 18.487 | not\_attempted |  |  |
| /Debugger/USB.VBUS | C5 pad 1 | U3 pad 5 | 18.348 | not\_attempted |  |  |
| /Debugger/TRG\_SWDIO\_NRF52 | U6 pad 6 | U5 pad 33 | 18.196 | not\_attempted |  |  |
| /nPM1300/NPM1300.SDA | U1 pad H2 | U2 pad 13 | 18.242 | not\_attempted |  |  |
| /Debugger/TRG\_SWDCLK\_NRF52 | U6 pad 7 | U5 pad 32 | 18.457 | not\_attempted |  |  |
| /nPM1300/NPM1300.SCL | U1 pad J2 | U2 pad 14 | 18.525 | not\_attempted |  |  |
| /Connectors/DIG.D13 | U1 pad J6 | J2 pad 4 | 19.008 | not\_attempted |  |  |
| /Connectors/ANALOG.A1 | U1 pad F8 | J1 pad 6 | 19.448 | not\_attempted |  |  |
| /nPM1300/NPM1300.INT | U1 pad K2 | U2 pad 7 | 19.307 | not\_attempted |  |  |
| /Connectors/ANALOG.A0 | U1 pad E9 | J1 pad 5 | 21.101 | not\_attempted |  |  |
| /Debugger/SWD\_TRG.~{RESET} | SW1 pad 2 | J1 pad 1 | 21.409 | not\_attempted |  |  |
| /Debugger/USB.CC2 | U2 pad 24 | J3 pad B5 | 20.963 | not\_attempted |  |  |
| /Debugger/USB.CC1 | U2 pad 23 | J3 pad A5 | 20.996 | not\_attempted |  |  |
| /Connectors/SHPHLD | U2 pad 15 | J1 pad 16 | 24.274 | not\_attempted |  |  |
| Net-(J6-Pin\_2) | U1 pad D9 | C14 pad 1 | 35.136 | not\_attempted |  |  |
| Net-(J6-Pin\_4) | U1 pad E8 | C13 pad 1 | 35.083 | not\_attempted |  |  |
| /Connectors/I2C.SCL | U1 pad G8 | R2 pad 2 | 36.019 | not\_attempted |  |  |
| /Connectors/I2C.SDA | U1 pad B3 | R3 pad 2 | 39.105 | not\_attempted |  |  |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (61[^11] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it.

| Missing connection | From | To | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of C25 \[/Debugger/nRF52\_VDD\] | Pad 1 of C10 \[/Debugger/nRF52\_VDD\] | 153.23 | 101.81 |
| Missing connection in net GND | Pad 2 of C25 \[GND\] | Pad 2 of J4 \[GND\] | 154.19 | 101.81 |
| Missing connection in net GND | Pad 2 of J4 \[GND\] | Pad A1 of U1 \[GND\] | 135.88 | 101.308 |
| Missing connection in net GND | Pad A1 of U1 \[GND\] | Pad A9 of U1 \[GND\] | 168.92 | 100.93 |
| Missing connection in net GND | Pad A9 of U1 \[GND\] | Pad 2 of C10 \[GND\] | 161.72 | 100.93 |
| Missing connection in net GND | Pad 2 of C10 \[GND\] | Pad 2 of U2 \[GND\] | 140.44 | 108.24 |
| Missing connection in net GND | Pad 2 of U2 \[GND\] | Pad 6 of U2 \[GND\] | 146.33 | 106.91 |
| Missing connection in net GND | Pad 6 of U2 \[GND\] | Pad 18 of U2 \[GND\] | 148.33 | 106.91 |
| Missing connection in net GND | Pad 18 of U2 \[GND\] | Pad 28 of U2 \[GND\] | 148.83 | 102.01 |
| Missing connection in net GND | Pad 28 of U2 \[GND\] | Pad 2 of C19 \[GND\] | 145.13 | 104.21 |
| Missing connection in net GND | Pad 2 of C19 \[GND\] | Pad A1 of J3 \[GND\] | 140.44 | 111.01 |
| Missing connection in net GND | Pad A1 of J3 \[GND\] | Pad 4 of Y2 \[GND\] | 125.405 | 101.73 |
| Missing connection in net GND | Pad 4 of Y2 \[GND\] | Pad 2 of C8 \[GND\] | 132.24 | 109.5 |
| Missing connection in net GND | Pad 2 of C8 \[GND\] | Pad 2 of C15 \[GND\] | 150.37 | 110.155 |
| Missing connection in net GND | Pad 2 of C15 \[GND\] | Pad 3 of U3 \[GND\] | 139.55 | 111.69 |
| Missing connection in net GND | Pad 3 of U3 \[GND\] | Pad 2 of C7 \[GND\] | 127.898 | 105.473 |
| Missing connection in net GND | Pad 2 of C7 \[GND\] | Pad 2 of C17 \[GND\] | 140.44 | 105.46 |
| Missing connection in net GND | Pad 2 of C17 \[GND\] | Pad 2 of C12 \[GND\] | 134.91 | 104.4 |
| Missing connection in net /nPM1300/NPM1300.SCL | Pad 2 of R5 \[/nPM1300/NPM1300.SCL\] | Pad 14 of U2 \[/nPM1300/NPM1300.SCL\] | 169.19 | 112.07 |
| Missing connection in net /Connectors/UART.RX | Pad A3 of U1 \[/Connectors/UART.RX\] | Pad 14 of U6 \[/Connectors/UART.RX\] | 167.12 | 100.93 |
| Missing connection in net /Connectors/UART.RX | Pad 14 of U6 \[/Connectors/UART.RX\] | Pad 14 of J1 \[/Connectors/UART.RX\] | 156.995 | 104.17 |
| Missing connection in net /Connectors/UART.TX | Pad B2 of U1 \[/Connectors/UART.TX\] | Pad 13 of U6 \[/Connectors/UART.TX\] | 168.02 | 101.83 |
| Missing connection in net /Connectors/UART.TX | Pad 13 of U6 \[/Connectors/UART.TX\] | Pad 15 of J1 \[/Connectors/UART.TX\] | 156.995 | 104.67 |
| Missing connection in net /Connectors/I2C.SDA | Pad B3 of U1 \[/Connectors/I2C.SDA\] | Pad 2 of R3 \[/Connectors/I2C.SDA\] | 167.12 | 101.83 |
| Missing connection in net /Connectors/ANALOG.A4 | Pad B4 of U1 \[/Connectors/ANALOG.A4\] | Pad 9 of J1 \[/Connectors/ANALOG.A4\] | 166.22 | 101.83 |
| Missing connection in net /Connectors/ANALOG.A5 | Pad B5 of U1 \[/Connectors/ANALOG.A5\] | Pad 10 of J1 \[/Connectors/ANALOG.A5\] | 165.32 | 101.83 |
| Missing connection in net Net-(J6-Pin\_2) | Pad D9 of U1 \[Net-(J6-Pin\_2)\] | Pad 2 of J6 \[Net-(J6-Pin\_2)\] | 161.72 | 103.63 |
| Missing connection in net Net-(J6-Pin\_4) | Pad E8 of U1 \[Net-(J6-Pin\_4)\] | Pad 1 of C13 \[Net-(J6-Pin\_4)\] | 162.62 | 104.53 |
| Missing connection in net /Connectors/ANALOG.A0 | Pad E9 of U1 \[/Connectors/ANALOG.A0\] | Pad 5 of J1 \[/Connectors/ANALOG.A0\] | 161.72 | 104.53 |
| Missing connection in net /Connectors/ANALOG.A1 | Pad F8 of U1 \[/Connectors/ANALOG.A1\] | Pad 6 of J1 \[/Connectors/ANALOG.A1\] | 162.62 | 105.43 |
| Missing connection in net /Connectors/ANALOG.A2 | Pad F9 of U1 \[/Connectors/ANALOG.A2\] | Pad 7 of J1 \[/Connectors/ANALOG.A2\] | 161.72 | 105.43 |
| Missing connection in net /Debugger/SWD\_TRG.~{RESET} | Pad G2 of U1 \[/Debugger/SWD\_TRG.~{RESET}\] | Pad 1 of J1 \[/Debugger/SWD\_TRG.~{RESET}\] | 168.02 | 106.33 |
| Missing connection in net /Connectors/I2C.SCL | Pad G8 of U1 \[/Connectors/I2C.SCL\] | Pad 2 of R2 \[/Connectors/I2C.SCL\] | 162.62 | 106.33 |
| Missing connection in net /Connectors/I2C.SCL | Pad 2 of R2 \[/Connectors/I2C.SCL\] | Pad 11 of J2 \[/Connectors/I2C.SCL\] | 127.103 | 100.335 |
| Missing connection in net /Connectors/ANALOG.A3 | Pad G9 of U1 \[/Connectors/ANALOG.A3\] | Pad 8 of J1 \[/Connectors/ANALOG.A3\] | 161.72 | 106.33 |
| Missing connection in net /nPM1300/NPM1300.SDA | Pad H2 of U1 \[/nPM1300/NPM1300.SDA\] | Pad 13 of U2 \[/nPM1300/NPM1300.SDA\] | 168.02 | 107.23 |
| Missing connection in net /Debugger/SWD\_TRG.SWDIO | Pad J3 of U1 \[/Debugger/SWD\_TRG.SWDIO\] | Pad 11 of U6 \[/Debugger/SWD\_TRG.SWDIO\] | 167.12 | 108.13 |
| Missing connection in net /Connectors/DIG.D11 | Pad J4 of U1 \[/Connectors/DIG.D11\] | Pad 1 of SW2 \[/Connectors/DIG.D11\] | 166.22 | 108.13 |
| Missing connection in net /Connectors/DIG.D12 | Pad J5 of U1 \[/Connectors/DIG.D12\] | Pad 5 of J2 \[/Connectors/DIG.D12\] | 165.32 | 108.13 |
| Missing connection in net /Connectors/DIG.D13 | Pad J6 of U1 \[/Connectors/DIG.D13\] | Pad 4 of J2 \[/Connectors/DIG.D13\] | 164.42 | 108.13 |
| Missing connection in net /nPM1300/NPM1300.INT | Pad K2 of U1 \[/nPM1300/NPM1300.INT\] | Pad 7 of U2 \[/nPM1300/NPM1300.INT\] | 168.02 | 109.03 |
| Missing connection in net /Debugger/SWD\_TRG.SWDCLK | Pad K3 of U1 \[/Debugger/SWD\_TRG.SWDCLK\] | Pad 10 of U6 \[/Debugger/SWD\_TRG.SWDCLK\] | 167.12 | 109.03 |
| Missing connection in net /Connectors/DIG.D6 | Pad K4 of U1 \[/Connectors/DIG.D6\] | Pad 9 of J2 \[/Connectors/DIG.D6\] | 166.22 | 109.03 |
| Missing connection in net /Connectors/DIG.D9 | Pad K5 of U1 \[/Connectors/DIG.D9\] | Pad 8 of J2 \[/Connectors/DIG.D9\] | 165.32 | 109.03 |
| Missing connection in net /Connectors/DIG.D10 | Pad K6 of U1 \[/Connectors/DIG.D10\] | Pad 7 of J2 \[/Connectors/DIG.D10\] | 164.42 | 109.03 |
| Missing connection in net /Connectors/ENABLE | Pad 8 of U2 \[/Connectors/ENABLE\] | Pad 2 of J2 \[/Connectors/ENABLE\] | 149.33 | 106.91 |
| Missing connection in net /Connectors/SHPHLD | Pad 15 of U2 \[/Connectors/SHPHLD\] | Pad 16 of J1 \[/Connectors/SHPHLD\] | 150.03 | 103.21 |
| Missing connection in net /Debugger/USB.VBUS | Pad 21 of U2 \[/Debugger/USB.VBUS\] | Pad A4 of J3 \[/Debugger/USB.VBUS\] | 147.33 | 102.01 |
| Missing connection in net /Debugger/USB.CC1 | Pad 23 of U2 \[/Debugger/USB.CC1\] | Pad A5 of J3 \[/Debugger/USB.CC1\] | 146.33 | 102.01 |
| Missing connection in net /Debugger/USB.CC2 | Pad 24 of U2 \[/Debugger/USB.CC2\] | Pad B5 of J3 \[/Debugger/USB.CC2\] | 145.83 | 102.01 |
| Missing connection in net Net-(U5-DEC4) | Pad 1 of C22 \[Net-(U5-DEC4)\] | Pad 26 of U5 \[Net-(U5-DEC4)\] | 133.26 | 106.78 |
| Missing connection in net Net-(U5-DEC4) | Pad 26 of U5 \[Net-(U5-DEC4)\] | Pad 38 of U5 \[Net-(U5-DEC4)\] | 134.443 | 108.33 |
| Missing connection in net /Debugger/TRG\_~{RESET}\_NRF52 | Pad 2 of R8 \[/Debugger/TRG\_~{RESET}\_NRF52\] | Pad 5 of U6 \[/Debugger/TRG\_~{RESET}\_NRF52\] | 143.11 | 108.03 |
| Missing connection in net /Debugger/TRG\_~{RESET}\_NRF52 | Pad 5 of U6 \[/Debugger/TRG\_~{RESET}\_NRF52\] | Pad 34 of U5 \[/Debugger/TRG\_~{RESET}\_NRF52\] | 153.405 | 105.17 |
| Missing connection in net /Debugger/UART\_RX\_NRF52 | Pad 3 of U6 \[/Debugger/UART\_RX\_NRF52\] | Pad 36 of U5 \[/Debugger/UART\_RX\_NRF52\] | 153.405 | 104.17 |
| Missing connection in net /Debugger/UART\_TX\_NRF52 | Pad 4 of U6 \[/Debugger/UART\_TX\_NRF52\] | Pad 35 of U5 \[/Debugger/UART\_TX\_NRF52\] | 153.405 | 104.67 |
| Missing connection in net /Debugger/TRG\_SWDIO\_NRF52 | Pad 6 of U6 \[/Debugger/TRG\_SWDIO\_NRF52\] | Pad 33 of U5 \[/Debugger/TRG\_SWDIO\_NRF52\] | 153.405 | 105.67 |
| Missing connection in net /Debugger/TRG\_SWDCLK\_NRF52 | Pad 7 of U6 \[/Debugger/TRG\_SWDCLK\_NRF52\] | Pad 32 of U5 \[/Debugger/TRG\_SWDCLK\_NRF52\] | 153.405 | 106.17 |
| Missing connection in net Net-(J8-~{RESET}) | Pad 16 of U5 \[Net-(J8-~{RESET})\] | Pad 1 of TP10 \[Net-(J8-~{RESET})\] | 136.68 | 105.693 |
| Missing connection in net Net-(J8-~{RESET}) | Pad 1 of TP10 \[Net-(J8-~{RESET})\] | Pad 3 of J8 \[Net-(J8-~{RESET})\] | 136.7 | 99.98 |
| Missing connection in net Net-(J8-SWDIO) | Pad 19 of U5 \[Net-(J8-SWDIO)\] | Pad 2 of J8 \[Net-(J8-SWDIO)\] | 135.48 | 105.693 |

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

Critical nets: 6[^12] nets, 2[^13] vias, 9[^14] crossings of a reference-plane void.

| Net | Tier | Why critical | Vias | Length, mm | Unrouted | Void crossings | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Net-(U1-P1.01{slash}XL2) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 5.175 | 0 | 0 |  |
| Net-(U1-P1.00{slash}XL1) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 16.951 | 0 | 1 | runs over 1 void of its reference plane (detour 3.20 mm) |
| Net-(U5-XC1) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 5.147 | 0 | 0 |  |
| Net-(U5-XC2) | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 3.959 | 0 | 0 |  |
| /Debugger/D+ | 2 | USB 2.0 (Full/High Speed) port (usb2 dp) | 1 | 20.534 | 0 | 4 | runs over 4 voids of its reference plane (detour 8.49 mm) |
| /Debugger/D- | 2 | USB 2.0 (Full/High Speed) port (usb2 dm) | 1 | 24.043 | 0 | 4 | runs over 4 voids of its reference plane (detour 8.59 mm) |

Rows: `/critical_nets/nets/<row>`

## DRC before and after

Checked with tracemaker drc[^15] (KiCad's violation names); zones refilled first: yes[^16]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 38 | 38 |
| Warnings | 0 | 0 |
| Unconnected items | 146 | 61 |

Rows: `/drc/before, /drc/after`

Errors added of types routing can cause: 0[^17]; violations gone: 0[^18].

| Type | Severity | Routing | Before | After | Added |
| --- | --- | --- | --- | --- | --- |
| clearance | error | yes | 10 | 10 | 0 |
| copper\_edge\_clearance | error | yes | 28 | 28 | 0 |

Rows: `/drc/types/<row>`

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
| --output | build/scratch/d3/RoyalBlue54L-Feather/routed.kicad\_pcb |
| --time | 3600 |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8841 |
| --json | build/scratch/d3/RoyalBlue54L-Feather/summary.json |
| --report | build/scratch/d3/RoyalBlue54L-Feather/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 53 | 7.165 |
| 1 | fast bends, shortest first | 102 | 15.668 |
| 2 | fast bends, longest first (2x pitch on large boards) | 25 | 14.131 |
| 3 | fast bends, jittered order, escape plan | 98 | 19.475 |
| 4 | exact bends, jittered order | 58 | 8.914 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 53 | 11.811 |
| 6 | fast bends, cheap crossings, escape plan | 101 | 16.027 |
| 7 | fast bends, dear vias | 102 | 15.915 |

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
