# Run report: unrouted.kicad\_pcb[^1]

TraceMaker 0.10.0[^2], 2026-10-24T01:41:45Z[^3]. Input build/demos/RoyalBlue54L-Feather//unrouted.kicad\_pcb[^4], output bench/results/run-report/RoyalBlue54L-Feather/routed.kicad\_pcb[^5].

> **110[^6] of 147[^7] connections are open; 25.17[^8] % are routed.**

> **After the zones are refilled for the new copper, 119[^9] items are unconnected: the board is not finished. See What is open and why.**

The route added 0[^10] DRC errors of types routing can cause.

## Result

|  | Value | JSON |
| --- | --- | --- |
| Connections | 147 | `/connections` |
| Routed | 37 | `/routed` |
| Open | 110 | `/open` |
| Routed, % | 25.17 | `/completion_pct` |
| Unconnected after zone refill | 119 | `/unconnected_after_refill` |
| Tracks added | 199 | `/tracks` |
| Vias added | 23 | `/vias` |
| Winning variant | fast bends, dear vias | `/variant_name` |
| Lattice pitch, mm | 0.05 | `/pitch_mm` |
| Search expansions, all variants | 29049979 | `/work_all_variants/total/expansions` |
| Wall time, s | 169.103 | `/seconds` |

## What is open and why

Open connections by cause:

| Cause | Connections | What it means |
| --- | --- | --- |
| not\_attempted | 107 | The run ended (work or time) before the router tried it again. |
| boxed\_in | 3 | Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it on the layers it can use. |

Rows: `/stuck/unrouted_by_reason/<row>`

> **Some pins cannot leave their package at all under the board's rules; no amount of routing effort connects them.**

| Part | Pitch, mm | Pins that cannot escape | Pads | Why | What would help |
| --- | --- | --- | --- | --- | --- |
| U5 | 0.4 | 2 | 12, 13 | no channel at 0.125 mm and no via site within reach |  |

Rows: `/stuck/escape_analysis/<row>/dead/<row>`

Parts with the most open connection ends:

| Part | Open ends |
| --- | --- |
| U1 | 29 |
| U5 | 24 |
| U2 | 20 |
| U6 | 11 |
| J1 | 8 |
| U4 | 8 |
| J3 | 6 |
| J2 | 5 |
| SW1 | 4 |
| D3 | 3 |
| J5 | 3 |
| J8 | 3 |
| L1 | 3 |
| R2 | 3 |
| R3 | 3 |
| SW2 | 3 |

Rows: `/stuck/unrouted_by_part/<row>`

Each open connection, with the router's last explanation and the nets whose copper was in the way:

| Net | From | To | Length, mm | Cause | Why | In the way |
| --- | --- | --- | --- | --- | --- | --- |
| Net-(J8-SWDIO) | U5 pad 19 | J8 pad 2 | 6.259 | boxed\_in | boxed in; escapes/neck-down: boxed in |  |
| /Debugger/TRG\_~{RESET}\_NRF52 | R8 pad 2 | U5 pad 34 | 7.286 | boxed\_in | boxed in (target); escapes/neck-down: boxed in (target) |  |
| /Debugger/UART\_RX\_NRF52 | U6 pad 3 | U5 pad 36 | 17.534 | boxed\_in | boxed in (target); escapes/neck-down: out of budget |  |
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
| VDD | R5 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | U1 pad B8 | zone fill | 0 | not\_attempted |  |  |
| VDD | U1 pad C8 | zone fill | 0 | not\_attempted |  |  |
| VDD | R7 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | U2 pad 12 | zone fill | 0 | not\_attempted |  |  |
| VDD | R2 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | C24 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | C1 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | U4 pad 8 | zone fill | 0 | not\_attempted |  |  |
| VDD | C9 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | U6 pad 15 | zone fill | 0 | not\_attempted |  |  |
| VDD | R3 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | J5 pad 2 | zone fill | 0 | not\_attempted |  |  |
| VDD | C12 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VDD | JP1 pad 2 | zone fill | 0 | not\_attempted |  |  |
| +BATT | J4 pad 1 | zone fill | 0 | not\_attempted |  |  |
| +BATT | U2 pad 19 | zone fill | 0 | not\_attempted |  |  |
| +BATT | C11 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VSYS | U2 pad 4 | zone fill | 0 | not\_attempted |  |  |
| VSYS | U2 pad 20 | zone fill | 0 | not\_attempted |  |  |
| VSYS | C4 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VSYS | C2 pad 1 | zone fill | 0 | not\_attempted |  |  |
| VBUS | U5 pad 10 | U5 pad 9 | 0.4 | not\_attempted |  |  |
| VSYS | C3 pad 1 | zone fill | 0 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U5 pad 40 | C15 pad 1 | 1.126 | not\_attempted |  |  |
| VBUS | C7 pad 1 | U5 pad 10 | 1.126 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | C10 pad 1 | U5 pad 8 | 1.132 | not\_attempted |  |  |
| Net-(U5-DEC1) | C19 pad 1 | U5 pad 1 | 1.129 | not\_attempted |  |  |
| Net-(U5-DECUSB) | C23 pad 1 | U5 pad 11 | 1.148 | not\_attempted |  |  |
| Net-(U2-VSET2) | U2 pad 16 | R4 pad 1 | 1.173 | not\_attempted |  |  |
| Net-(JP1-A) | L1 pad 1 | C8 pad 1 | 1.51 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U5 pad 18 | C17 pad 1 | 1.293 | not\_attempted |  |  |
| Net-(D1-K) | R6 pad 1 | D1 pad 1 | 1.303 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U6 pad 8 | R1 pad 1 | 1.34 | not\_attempted |  |  |
| /Connectors/I2C.SDA | R3 pad 2 | J5 pad 3 | 1.79 | not\_attempted |  |  |
| /Connectors/I2C.SCL | R2 pad 2 | J5 pad 4 | 1.809 | not\_attempted |  |  |
| Net-(J6-Pin\_2) | C14 pad 1 | J6 pad 2 | 1.425 | not\_attempted |  |  |
| Net-(J6-Pin\_4) | C13 pad 1 | J6 pad 4 | 1.425 | not\_attempted |  |  |
| VSYS | U2 pad 1 | C3 pad 1 | 1.927 | not\_attempted |  |  |
| Net-(U5-DEC3) | C21 pad 1 | U5 pad 27 | 1.935 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | C16 pad 1 | U5 pad 30 | 1.78 | not\_attempted |  |  |
| /nRF54L15/D3\_FLASH | U1 pad H9 | U4 pad 7 | 1.903 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | C25 pad 1 | U6 pad 2 | 1.868 | not\_attempted |  |  |
| /Debugger/USB.VBUS | U2 pad 21 | C5 pad 1 | 2.436 | not\_attempted |  |  |
| /Debugger/USB.VBUS | J3 pad A4 | TP5 pad 1 | 3.311 | not\_attempted |  |  |
| /Connectors/SPI.SCK | U1 pad J9 | U4 pad 6 | 2.267 | not\_attempted |  |  |
| Net-(D1-A) | U1 pad K7 | D1 pad 2 | 2.676 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | R8 pad 1 | C10 pad 1 | 2.684 | not\_attempted |  |  |
| Net-(D3-RK) | D3 pad 2 | U2 pad 25 | 2.63 | not\_attempted |  |  |
| /Debugger/USB.CC2 | J3 pad B5 | TP6 pad 1 | 3.614 | not\_attempted |  |  |
| VBUS | U2 pad 22 | C6 pad 1 | 3.104 | not\_attempted |  |  |
| Net-(J8-SWDIO) | U5 pad 19 | TP12 pad 1 | 3.53 | not\_attempted |  |  |
| /Debugger/SWD\_TRG.SWDCLK | U1 pad K3 | TP3 pad 1 | 3.592 | not\_attempted |  |  |
| Net-(U2-SW2) | U2 pad 5 | L1 pad 2 | 3.393 | not\_attempted |  |  |
| Net-(D3-BK) | D3 pad 4 | U2 pad 27 | 3.132 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U6 pad 2 | U6 pad 8 | 3.457 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U5 pad 8 | U5 pad 40 | 3.496 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U5 pad 8 | U5 pad 18 | 3.726 | not\_attempted |  |  |
| /Debugger/SWD\_TRG.~{RESET} | SW1 pad 2 | SW1 pad 2 | 4.125 | not\_attempted |  |  |
| /Connectors/DIG.D11 | SW2 pad 1 | SW2 pad 1 | 4.15 | not\_attempted |  |  |
| /Debugger/USB.VBUS | U3 pad 5 | J3 pad A4 | 4.373 | not\_attempted |  |  |
| VSYS | C4 pad 1 | J2 pad 3 | 4.783 | not\_attempted |  |  |
| /Debugger/SWD\_TRG.~{RESET} | U1 pad G2 | TP2 pad 1 | 4.68 | not\_attempted |  |  |
| /nPM1300/NPM1300.SDA | U1 pad H2 | R7 pad 2 | 4.072 | not\_attempted |  |  |
| VSYS | D3 pad 1 | U2 pad 1 | 4.081 | not\_attempted |  |  |
| /nPM1300/NPM1300.SCL | R5 pad 2 | U1 pad J2 | 4.11 | not\_attempted |  |  |
| /Connectors/SPI.MISO | U1 pad K9 | U4 pad 2 | 4.302 | not\_attempted |  |  |
| /Connectors/SPI.MOSI | U1 pad J8 | U4 pad 5 | 3.914 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | C15 pad 1 | C16 pad 1 | 4.15 | not\_attempted |  |  |
| /Debugger/SWD\_TRG.~{RESET} | U6 pad 12 | SW1 pad 2 | 4.367 | not\_attempted |  |  |
| /Debugger/USB.VBUS | U3 pad 5 | J3 pad A9 | 4.909 | not\_attempted |  |  |
| /nRF54L15/~{CS}\_FLASH | U1 pad K8 | U4 pad 1 | 4.606 | not\_attempted |  |  |
| Net-(J8-SWCLK) | U5 pad 20 | TP11 pad 1 | 5.209 | not\_attempted |  |  |
| Net-(JP1-A) | C8 pad 1 | JP1 pad 1 | 5.341 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | C17 pad 1 | TP9 pad 1 | 5.474 | not\_attempted |  |  |
| /Connectors/DIG.D11 | SW2 pad 1 | J2 pad 6 | 5.996 | not\_attempted |  |  |
| /nRF54L15/D2\_FLASH | U1 pad J7 | U4 pad 3 | 5.266 | not\_attempted |  |  |
| /Debugger/SWD\_TRG.SWDIO | U1 pad J3 | TP1 pad 1 | 6.513 | not\_attempted |  |  |
| /Connectors/SPI.MISO | U1 pad K9 | J1 pad 13 | 6.378 | not\_attempted |  |  |
| Net-(J8-SWCLK) | U5 pad 20 | J8 pad 4 | 5.879 | not\_attempted |  |  |
| Net-(JP1-A) | U2 pad 32 | L1 pad 1 | 6.652 | not\_attempted |  |  |
| /Connectors/SPI.SCK | U4 pad 6 | J1 pad 11 | 7.223 | not\_attempted |  |  |
| /Connectors/SPI.MOSI | U1 pad J8 | J1 pad 12 | 7.193 | not\_attempted |  |  |
| /Connectors/DIG.D5 | U1 pad B6 | J2 pad 10 | 7.206 | not\_attempted |  |  |
| /Debugger/nRF52\_VDD | U5 pad 30 | J8 pad 1 | 7.445 | not\_attempted |  |  |
| /Connectors/I2C.SDA | U1 pad B3 | J2 pad 12 | 8.096 | not\_attempted |  |  |
| VBUS | U2 pad 22 | C7 pad 1 | 7.764 | not\_attempted |  |  |
| /Debugger/SWD\_TRG.~{RESET} | TP2 pad 1 | U6 pad 12 | 8.787 | not\_attempted |  |  |

Rows: `/stuck/unrouted/<row>`

Unconnected on the board as written, by the DRC after the route (119[^11] items). With copper zones this is the number that counts: the zones are refilled for the new copper first, and a fill the new tracks cut off no longer joins the pads the router counted as joined through it.

| Missing connection | From | To | X, mm | Y, mm |
| --- | --- | --- | --- | --- |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of C25 \[/Debugger/nRF52\_VDD\] | Pad 1 of C10 \[/Debugger/nRF52\_VDD\] | 153.23 | 101.81 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of C10 \[/Debugger/nRF52\_VDD\] | Pad 1 of C16 \[/Debugger/nRF52\_VDD\] | 140.44 | 107.28 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of C16 \[/Debugger/nRF52\_VDD\] | Pad 1 of R8 \[/Debugger/nRF52\_VDD\] | 134.44 | 111.71 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of R8 \[/Debugger/nRF52\_VDD\] | Pad 8 of U6 \[/Debugger/nRF52\_VDD\] | 143.11 | 107.01 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 8 of U6 \[/Debugger/nRF52\_VDD\] | Pad 1 of C15 \[/Debugger/nRF52\_VDD\] | 154.45 | 106.965 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of C15 \[/Debugger/nRF52\_VDD\] | Pad 1 of R1 \[/Debugger/nRF52\_VDD\] | 138.59 | 111.69 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of R1 \[/Debugger/nRF52\_VDD\] | Pad 1 of C17 \[/Debugger/nRF52\_VDD\] | 155.25 | 108.04 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of C17 \[/Debugger/nRF52\_VDD\] | Pad 8 of U5 \[/Debugger/nRF52\_VDD\] | 135.87 | 104.4 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 8 of U5 \[/Debugger/nRF52\_VDD\] | Pad 18 of U5 \[/Debugger/nRF52\_VDD\] | 139.318 | 107.13 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 18 of U5 \[/Debugger/nRF52\_VDD\] | Pad 30 of U5 \[/Debugger/nRF52\_VDD\] | 135.88 | 105.693 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 30 of U5 \[/Debugger/nRF52\_VDD\] | Pad 40 of U5 \[/Debugger/nRF52\_VDD\] | 134.443 | 109.93 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 40 of U5 \[/Debugger/nRF52\_VDD\] | Pad 1 of TP9 \[/Debugger/nRF52\_VDD\] | 138.68 | 110.568 |
| Missing connection in net /Debugger/nRF52\_VDD | Pad 1 of TP9 \[/Debugger/nRF52\_VDD\] | Pad 1 of J8 \[/Debugger/nRF52\_VDD\] | 139.1 | 99.98 |
| Missing connection in net GND | Pad 2 of C25 \[GND\] | Pad H8 of U1 \[GND\] | 154.19 | 101.81 |
| Missing connection in net GND | Pad H8 of U1 \[GND\] | Pad 2 of U2 \[GND\] | 162.62 | 107.23 |
| Missing connection in net GND | Pad 2 of U2 \[GND\] | Pad 6 of U2 \[GND\] | 146.33 | 106.91 |
| Missing connection in net GND | Pad 6 of U2 \[GND\] | Pad 28 of U2 \[GND\] | 148.33 | 106.91 |
| Missing connection in net GND | Pad 28 of U2 \[GND\] | Pad 2 of C1 \[GND\] | 145.13 | 104.21 |
| Missing connection in net GND | Pad 2 of C1 \[GND\] | Pad 4 of U4 \[GND\] | 151.14 | 106.17 |
| Missing connection in net GND | Pad 4 of U4 \[GND\] | Pad 4 of Y2 \[GND\] | 158.71 | 105.105 |
| Missing connection in net GND | Pad 4 of Y2 \[GND\] | Pad 2 of C11 \[GND\] | 132.24 | 109.5 |
| Missing connection in net GND | Pad 2 of C11 \[GND\] | Pad 3 of U3 \[GND\] | 151.595 | 100.59 |
| Missing connection in net GND | Pad 3 of U3 \[GND\] | Pad 2 of C17 \[GND\] | 127.898 | 105.473 |
| Missing connection in net VDD | Pad 1 of R5 \[VDD\] | Pad B8 of U1 \[VDD\] | 170.21 | 112.07 |
| Missing connection in net VDD | Pad B8 of U1 \[VDD\] | Pad C8 of U1 \[VDD\] | 162.62 | 101.83 |
| Missing connection in net VDD | Pad C8 of U1 \[VDD\] | Pad 1 of R7 \[VDD\] | 162.62 | 102.73 |
| Missing connection in net VDD | Pad 1 of R7 \[VDD\] | Pad 12 of U2 \[VDD\] | 170.21 | 111.13 |
| Missing connection in net VDD | Pad 12 of U2 \[VDD\] | Pad 1 of R2 \[VDD\] | 150.03 | 104.71 |
| Missing connection in net VDD | Pad 1 of R2 \[VDD\] | Pad 1 of C24 \[VDD\] | 127.103 | 101.355 |
| Missing connection in net VDD | Pad 1 of C24 \[VDD\] | Pad 1 of C1 \[VDD\] | 158.06 | 103.42 |
| Missing connection in net VDD | Pad 1 of C1 \[VDD\] | Pad 8 of U4 \[VDD\] | 151.14 | 105.21 |
| Missing connection in net VDD | Pad 8 of U4 \[VDD\] | Pad 1 of C9 \[VDD\] | 160.21 | 107.955 |
| Missing connection in net VDD | Pad 1 of C9 \[VDD\] | Pad 15 of U6 \[VDD\] | 160.26 | 100.87 |
| Missing connection in net VDD | Pad 15 of U6 \[VDD\] | Pad 1 of R3 \[VDD\] | 156.995 | 103.67 |
| Missing connection in net VDD | Pad 1 of R3 \[VDD\] | Pad 2 of J5 \[VDD\] | 128.043 | 101.355 |
| Missing connection in net VDD | Pad 2 of J5 \[VDD\] | Pad 1 of C12 \[VDD\] | 128.49 | 98.633 |
| Missing connection in net VDD | Pad 1 of C12 \[VDD\] | Pad 2 of JP1 \[VDD\] | 160.21 | 109.14 |
| Missing connection in net VDD | Pad 2 of JP1 \[VDD\] | Pad 2 of J1 \[VDD\] | 152.86 | 105.68 |
| Missing connection in net /nPM1300/NPM1300.SCL | Pad 2 of R5 \[/nPM1300/NPM1300.SCL\] | Pad J2 of U1 \[/nPM1300/NPM1300.SCL\] | 169.19 | 112.07 |
| Missing connection in net /nPM1300/NPM1300.SCL | Pad J2 of U1 \[/nPM1300/NPM1300.SCL\] | Pad 14 of U2 \[/nPM1300/NPM1300.SCL\] | 168.02 | 108.13 |
| Missing connection in net +BATT | Pad 1 of J4 \[+BATT\] | Pad 19 of U2 \[+BATT\] | 137.88 | 101.308 |
| Missing connection in net +BATT | Pad 19 of U2 \[+BATT\] | Pad 1 of C11 \[+BATT\] | 148.33 | 102.01 |
| Missing connection in net +BATT | Pad 1 of C11 \[+BATT\] | Pad 1 of J2 \[+BATT\] | 150.045 | 100.59 |
| Missing connection in net /Connectors/I2C.SDA | Pad B3 of U1 \[/Connectors/I2C.SDA\] | Pad 12 of J2 \[/Connectors/I2C.SDA\] | 167.12 | 101.83 |
| Missing connection in net /Connectors/I2C.SDA | Pad 12 of J2 \[/Connectors/I2C.SDA\] | Pad 2 of R3 \[/Connectors/I2C.SDA\] | 171.17 | 94.82 |
| Missing connection in net /Connectors/I2C.SDA | Pad 2 of R3 \[/Connectors/I2C.SDA\] | Pad 3 of J5 \[/Connectors/I2C.SDA\] | 128.043 | 100.335 |
| Missing connection in net /Connectors/ANALOG.A4 | Pad B4 of U1 \[/Connectors/ANALOG.A4\] | Pad 9 of J1 \[/Connectors/ANALOG.A4\] | 166.22 | 101.83 |
| Missing connection in net /Connectors/DIG.D5 | Pad B6 of U1 \[/Connectors/DIG.D5\] | Pad 10 of J2 \[/Connectors/DIG.D5\] | 164.42 | 101.83 |
| Missing connection in net Net-(J6-Pin\_2) | Pad D9 of U1 \[Net-(J6-Pin\_2)\] | Pad 2 of J6 \[Net-(J6-Pin\_2)\] | 161.72 | 103.63 |
| Missing connection in net Net-(J6-Pin\_2) | Pad 2 of J6 \[Net-(J6-Pin\_2)\] | Pad 1 of C14 \[Net-(J6-Pin\_2)\] | 127.31 | 113.28 |
| Missing connection in net Net-(J6-Pin\_4) | Pad E8 of U1 \[Net-(J6-Pin\_4)\] | Pad 1 of C13 \[Net-(J6-Pin\_4)\] | 162.62 | 104.53 |
| Missing connection in net Net-(J6-Pin\_4) | Pad 1 of C13 \[Net-(J6-Pin\_4)\] | Pad 4 of J6 \[Net-(J6-Pin\_4)\] | 128.31 | 111.855 |
| Missing connection in net /Connectors/ANALOG.A0 | Pad E9 of U1 \[/Connectors/ANALOG.A0\] | Pad 5 of J1 \[/Connectors/ANALOG.A0\] | 161.72 | 104.53 |
| Missing connection in net /Connectors/ANALOG.A1 | Pad F8 of U1 \[/Connectors/ANALOG.A1\] | Pad 6 of J1 \[/Connectors/ANALOG.A1\] | 162.62 | 105.43 |
| Missing connection in net /Debugger/SWD\_TRG.~{RESET} | Pad G2 of U1 \[/Debugger/SWD\_TRG.~{RESET}\] | Pad 2 of SW1 \[/Debugger/SWD\_TRG.~{RESET}\] | 168.02 | 106.33 |
| Missing connection in net /Debugger/SWD\_TRG.~{RESET} | Pad 2 of SW1 \[/Debugger/SWD\_TRG.~{RESET}\] | Pad 2 of SW1 \[/Debugger/SWD\_TRG.~{RESET}\] | 157.835 | 109.455 |
| Missing connection in net /Debugger/SWD\_TRG.~{RESET} | Pad 2 of SW1 \[/Debugger/SWD\_TRG.~{RESET}\] | Pad 12 of U6 \[/Debugger/SWD\_TRG.~{RESET}\] | 153.71 | 109.455 |
| Missing connection in net /Debugger/SWD\_TRG.~{RESET} | Pad 12 of U6 \[/Debugger/SWD\_TRG.~{RESET}\] | Pad 1 of J1 \[/Debugger/SWD\_TRG.~{RESET}\] | 156.995 | 105.17 |
| Missing connection in net /Debugger/SWD\_TRG.~{RESET} | Pad 1 of J1 \[/Debugger/SWD\_TRG.~{RESET}\] | Pad 1 of TP2 \[/Debugger/SWD\_TRG.~{RESET}\] | 133.07 | 115.14 |
| Missing connection in net /Connectors/I2C.SCL | Pad G8 of U1 \[/Connectors/I2C.SCL\] | Pad 2 of R2 \[/Connectors/I2C.SCL\] | 162.62 | 106.33 |
| Missing connection in net /Connectors/I2C.SCL | Pad 2 of R2 \[/Connectors/I2C.SCL\] | Pad 4 of J5 \[/Connectors/I2C.SCL\] | 127.103 | 100.335 |
| Missing connection in net /nPM1300/NPM1300.SDA | Pad H2 of U1 \[/nPM1300/NPM1300.SDA\] | Pad 2 of R7 \[/nPM1300/NPM1300.SDA\] | 168.02 | 107.23 |
| Missing connection in net /nPM1300/NPM1300.SDA | Pad 2 of R7 \[/nPM1300/NPM1300.SDA\] | Pad 13 of U2 \[/nPM1300/NPM1300.SDA\] | 169.19 | 111.13 |
| Missing connection in net /nRF54L15/D3\_FLASH | Pad H9 of U1 \[/nRF54L15/D3\_FLASH\] | Pad 7 of U4 \[/nRF54L15/D3\_FLASH\] | 161.72 | 107.23 |
| Missing connection in net /Debugger/SWD\_TRG.SWDIO | Pad J3 of U1 \[/Debugger/SWD\_TRG.SWDIO\] | Pad 1 of TP1 \[/Debugger/SWD\_TRG.SWDIO\] | 167.12 | 108.13 |
| Missing connection in net /Connectors/DIG.D11 | Pad J4 of U1 \[/Connectors/DIG.D11\] | Pad 1 of SW2 \[/Connectors/DIG.D11\] | 166.22 | 108.13 |
| Missing connection in net /Connectors/DIG.D11 | Pad 1 of SW2 \[/Connectors/DIG.D11\] | Pad 6 of J2 \[/Connectors/DIG.D11\] | 153.685 | 100.505 |
| Missing connection in net /Connectors/DIG.D13 | Pad J6 of U1 \[/Connectors/DIG.D13\] | Pad 4 of J2 \[/Connectors/DIG.D13\] | 164.42 | 108.13 |
| Missing connection in net /nRF54L15/D2\_FLASH | Pad J7 of U1 \[/nRF54L15/D2\_FLASH\] | Pad 3 of U4 \[/nRF54L15/D2\_FLASH\] | 163.52 | 108.13 |
| Missing connection in net /Connectors/SPI.MOSI | Pad J8 of U1 \[/Connectors/SPI.MOSI\] | Pad 5 of U4 \[/Connectors/SPI.MOSI\] | 162.62 | 108.13 |
| Missing connection in net /Connectors/SPI.MOSI | Pad 5 of U4 \[/Connectors/SPI.MOSI\] | Pad 12 of J1 \[/Connectors/SPI.MOSI\] | 158.71 | 107.955 |
| Missing connection in net /Connectors/SPI.SCK | Pad J9 of U1 \[/Connectors/SPI.SCK\] | Pad 6 of U4 \[/Connectors/SPI.SCK\] | 161.72 | 108.13 |
| Missing connection in net /Connectors/SPI.SCK | Pad 6 of U4 \[/Connectors/SPI.SCK\] | Pad 11 of J1 \[/Connectors/SPI.SCK\] | 159.21 | 107.955 |
| Missing connection in net /nPM1300/NPM1300.INT | Pad K2 of U1 \[/nPM1300/NPM1300.INT\] | Pad 7 of U2 \[/nPM1300/NPM1300.INT\] | 168.02 | 109.03 |
| Missing connection in net /Debugger/SWD\_TRG.SWDCLK | Pad K3 of U1 \[/Debugger/SWD\_TRG.SWDCLK\] | Pad 1 of TP3 \[/Debugger/SWD\_TRG.SWDCLK\] | 167.12 | 109.03 |
| Missing connection in net Net-(D1-A) | Pad K7 of U1 \[Net-(D1-A)\] | Pad 2 of D1 \[Net-(D1-A)\] | 163.52 | 109.03 |
| Missing connection in net /nRF54L15/~{CS}\_FLASH | Pad K8 of U1 \[/nRF54L15/~{CS}\_FLASH\] | Pad 1 of U4 \[/nRF54L15/~{CS}\_FLASH\] | 162.62 | 109.03 |
| Missing connection in net /Connectors/SPI.MISO | Pad K9 of U1 \[/Connectors/SPI.MISO\] | Pad 2 of U4 \[/Connectors/SPI.MISO\] | 161.72 | 109.03 |
| Missing connection in net /Connectors/SPI.MISO | Pad 2 of U4 \[/Connectors/SPI.MISO\] | Pad 13 of J1 \[/Connectors/SPI.MISO\] | 159.71 | 105.105 |
| Missing connection in net Net-(D1-K) | Pad 1 of R6 \[Net-(D1-K)\] | Pad 1 of D1 \[Net-(D1-K)\] | 166.94 | 111.65 |
| Missing connection in net VSYS | Pad 1 of D3 \[VSYS\] | Pad 1 of U2 \[VSYS\] | 142.48 | 104.58 |
| Missing connection in net VSYS | Pad 1 of U2 \[VSYS\] | Pad 4 of U2 \[VSYS\] | 145.83 | 106.91 |
| Missing connection in net VSYS | Pad 4 of U2 \[VSYS\] | Pad 20 of U2 \[VSYS\] | 147.33 | 106.91 |
| Missing connection in net VSYS | Pad 20 of U2 \[VSYS\] | Pad 1 of C4 \[VSYS\] | 147.83 | 102.01 |
| Missing connection in net VSYS | Pad 1 of C4 \[VSYS\] | Pad 3 of J2 \[VSYS\] | 148.58 | 99.595 |
| Missing connection in net VSYS | Pad 3 of J2 \[VSYS\] | Pad 1 of C2 \[VSYS\] | 148.31 | 94.82 |
| Missing connection in net VSYS | Pad 1 of C2 \[VSYS\] | Pad 1 of C3 \[VSYS\] | 148.085 | 108.67 |
| Missing connection in net Net-(D3-RK) | Pad 2 of D3 \[Net-(D3-RK)\] | Pad 25 of U2 \[Net-(D3-RK)\] | 143.28 | 104.58 |
| Missing connection in net Net-(D3-BK) | Pad 4 of D3 \[Net-(D3-BK)\] | Pad 27 of U2 \[Net-(D3-BK)\] | 142.48 | 105.38 |
| Missing connection in net Net-(U2-SW2) | Pad 5 of U2 \[Net-(U2-SW2)\] | Pad 2 of L1 \[Net-(U2-SW2)\] | 147.83 | 106.91 |
| Missing connection in net /Connectors/SHPHLD | Pad 15 of U2 \[/Connectors/SHPHLD\] | Pad 16 of J1 \[/Connectors/SHPHLD\] | 150.03 | 103.21 |
| Missing connection in net Net-(U2-VSET2) | Pad 16 of U2 \[Net-(U2-VSET2)\] | Pad 1 of R4 \[Net-(U2-VSET2)\] | 150.03 | 102.71 |
| Missing connection in net /Debugger/USB.VBUS | Pad 21 of U2 \[/Debugger/USB.VBUS\] | Pad A4 of J3 \[/Debugger/USB.VBUS\] | 147.33 | 102.01 |
| Missing connection in net /Debugger/USB.VBUS | Pad A4 of J3 \[/Debugger/USB.VBUS\] | Pad A9 of J3 \[/Debugger/USB.VBUS\] | 125.405 | 102.53 |
| Missing connection in net /Debugger/USB.VBUS | Pad A9 of J3 \[/Debugger/USB.VBUS\] | Pad 5 of U3 \[/Debugger/USB.VBUS\] | 125.405 | 107.43 |
| Missing connection in net /Debugger/USB.VBUS | Pad 5 of U3 \[/Debugger/USB.VBUS\] | Pad 1 of C5 \[/Debugger/USB.VBUS\] | 129.322 | 104.473 |
| Missing connection in net /Debugger/USB.VBUS | Pad 1 of C5 \[/Debugger/USB.VBUS\] | Pad 1 of TP5 \[/Debugger/USB.VBUS\] | 147.01 | 99.595 |
| Missing connection in net VBUS | Pad 22 of U2 \[VBUS\] | Pad 1 of C6 \[VBUS\] | 146.83 | 102.01 |
| Missing connection in net VBUS | Pad 1 of C6 \[VBUS\] | Pad 1 of C7 \[VBUS\] | 144.88 | 99.595 |
| Missing connection in net VBUS | Pad 1 of C7 \[VBUS\] | Pad 9 of U5 \[VBUS\] | 140.44 | 106.42 |
| Missing connection in net VBUS | Pad 9 of U5 \[VBUS\] | Pad 10 of U5 \[VBUS\] | 139.318 | 106.73 |
| Missing connection in net /Debugger/USB.CC1 | Pad 23 of U2 \[/Debugger/USB.CC1\] | Pad A5 of J3 \[/Debugger/USB.CC1\] | 146.33 | 102.01 |
| Missing connection in net /Debugger/USB.CC2 | Pad 24 of U2 \[/Debugger/USB.CC2\] | Pad B5 of J3 \[/Debugger/USB.CC2\] | 145.83 | 102.01 |
| Missing connection in net /Debugger/USB.CC2 | Pad B5 of J3 \[/Debugger/USB.CC2\] | Pad 1 of TP6 \[/Debugger/USB.CC2\] | 125.405 | 106.73 |
| Missing connection in net Net-(JP1-A) | Pad 32 of U2 \[Net-(JP1-A)\] | Pad 1 of C8 \[Net-(JP1-A)\] | 145.13 | 106.21 |
| Missing connection in net Net-(JP1-A) | Pad 1 of C8 \[Net-(JP1-A)\] | Pad 1 of L1 \[Net-(JP1-A)\] | 150.37 | 111.705 |
| Missing connection in net Net-(JP1-A) | Pad 1 of L1 \[Net-(JP1-A)\] | Pad 2 of JP1 \[VDD\] | 148.86 | 111.718 |
| Missing connection in net Net-(U5-DECUSB) | Pad 1 of C23 \[Net-(U5-DECUSB)\] | Pad 11 of U5 \[Net-(U5-DECUSB)\] | 138.92 | 104.57 |
| Missing connection in net Net-(U5-DEC1) | Pad 1 of C19 \[Net-(U5-DEC1)\] | Pad 1 of U5 \[Net-(U5-DEC1)\] | 140.44 | 110.05 |
| Missing connection in net /Debugger/TRG\_~{RESET}\_NRF52 | Pad 2 of R8 \[/Debugger/TRG\_~{RESET}\_NRF52\] | Pad 34 of U5 \[/Debugger/TRG\_~{RESET}\_NRF52\] | 143.11 | 108.03 |
| Missing connection in net /Debugger/UART\_RX\_NRF52 | Pad 3 of U6 \[/Debugger/UART\_RX\_NRF52\] | Pad 36 of U5 \[/Debugger/UART\_RX\_NRF52\] | 153.405 | 104.17 |
| Missing connection in net /Debugger/UART\_TX\_NRF52 | Pad 4 of U6 \[/Debugger/UART\_TX\_NRF52\] | Pad 35 of U5 \[/Debugger/UART\_TX\_NRF52\] | 153.405 | 104.67 |
| Missing connection in net /Debugger/TRG\_SWDIO\_NRF52 | Pad 6 of U6 \[/Debugger/TRG\_SWDIO\_NRF52\] | Pad 33 of U5 \[/Debugger/TRG\_SWDIO\_NRF52\] | 153.405 | 105.67 |
| Missing connection in net /Debugger/TRG\_SWDCLK\_NRF52 | Pad 7 of U6 \[/Debugger/TRG\_SWDCLK\_NRF52\] | Pad 32 of U5 \[/Debugger/TRG\_SWDCLK\_NRF52\] | 153.405 | 106.17 |
| Missing connection in net Net-(U5-DEC3) | Pad 1 of C21 \[Net-(U5-DEC3)\] | Pad 27 of U5 \[Net-(U5-DEC3)\] | 132.78 | 107.74 |
| Missing connection in net Net-(J8-SWDIO) | Pad 19 of U5 \[Net-(J8-SWDIO)\] | Pad 1 of TP12 \[Net-(J8-SWDIO)\] | 135.48 | 105.693 |
| Missing connection in net Net-(J8-SWDIO) | Pad 1 of TP12 \[Net-(J8-SWDIO)\] | Pad 2 of J8 \[Net-(J8-SWDIO)\] | 136.7 | 102.38 |
| Missing connection in net Net-(J8-SWCLK) | Pad 20 of U5 \[Net-(J8-SWCLK)\] | Pad 1 of TP11 \[Net-(J8-SWCLK)\] | 135.08 | 105.693 |
| Missing connection in net Net-(J8-SWCLK) | Pad 1 of TP11 \[Net-(J8-SWCLK)\] | Pad 4 of J8 \[Net-(J8-SWCLK)\] | 139.1 | 102.38 |

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

Critical nets: 6[^12] nets, 2[^13] vias, 7[^14] crossings of a reference-plane void.

| Net | Tier | Why critical | Vias | Length, mm | Unrouted | Void crossings | Warnings |
| --- | --- | --- | --- | --- | --- | --- | --- |
| Net-(U1-P1.01{slash}XL2) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 5.175 | 0 | 0 |  |
| Net-(U1-P1.00{slash}XL1) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 16.968 | 0 | 1 | runs over 1 void of its reference plane (detour 5.84 mm) |
| Net-(U5-XC1) | 1 | Quartz crystal with load capacitors (crystal xin) | 0 | 5.147 | 0 | 1 | runs over 1 void of its reference plane (detour 4.47 mm) |
| Net-(U5-XC2) | 1 | Quartz crystal with load capacitors (crystal xout) | 0 | 3.959 | 0 | 1 | runs over 1 void of its reference plane (detour 6.62 mm) |
| /Debugger/D+ | 2 | USB 2.0 (Full/High Speed) port (usb2 dp) | 1 | 20.534 | 0 | 2 | runs over 2 voids of its reference plane (detour 0.82 mm) |
| /Debugger/D- | 2 | USB 2.0 (Full/High Speed) port (usb2 dm) | 1 | 24.043 | 0 | 2 | runs over 2 voids of its reference plane (detour 1.02 mm) |

Rows: `/critical_nets/nets/<row>`

## DRC before and after

Checked with tracemaker drc[^15] (KiCad's violation names); zones refilled first: yes[^16]. The comparison counts violations by type; sign off with kicad-cli pcb drc.

|  | Before | After |
| --- | --- | --- |
| Errors | 38 | 38 |
| Warnings | 0 | 0 |
| Unconnected items | 146 | 119 |

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
| Time limit, s | 120 | `/run/time_limit_s` |
| Variants | 8 | `/run/variants` |
| Threads | 4 | `/run/threads` |

Options given (command line and --config):

| Option | Value |
| --- | --- |
| --output | bench/results/run-report/RoyalBlue54L-Feather/routed.kicad\_pcb |
| --work | 3000000 |
| --soft-zones | true |
| --threads | 4 |
| --view | true |
| --si-rules | true |
| --view-port | 8804 |
| --json | bench/results/run-report/RoyalBlue54L-Feather/summary.json |
| --report | bench/results/run-report/RoyalBlue54L-Feather/report.html |

Rows: `/run/options/<row>`

Portfolio variants:

| Variant | Name | Routed | Seconds |
| --- | --- | --- | --- |
| 0 | exact bends, shortest first | 18 | 43.138 |
| 1 | fast bends, shortest first | 26 | 23.435 |
| 2 | fast bends, longest first (2x pitch on large boards) | 25 | 99.472 |
| 3 | fast bends, jittered order, escape plan | 29 | 31.673 |
| 4 | exact bends, jittered order | 17 | 30.753 |
| 5 | fast bends, cheap vias (2x pitch on large boards, half pitch on small ones) | 17 | 15.204 |
| 6 | fast bends, cheap crossings, escape plan | 26 | 121.898 |
| 7 | fast bends, dear vias | 37 | 20.994 |

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
