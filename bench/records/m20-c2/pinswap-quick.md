# Pin-swap oracle (c2q-pinswap)

Runs `c2q-base` (as designed) and `c2q-swap` (the oracle's nets), same options and work; KiCad judges both.

| Board | Groups | Pads changed | Ratsnest in groups (mm) | Routed | Vias | Length (mm) | Clean |
|---|--:|--:|---|---|---|---|---|
| ESP8266-MQTT-battery-monitor-hw_battery-monitor | 1 | 2 | 41.6 → 41.3 | 95/95 → 95/95 | 36 → 36 | 549.1 → 548.0 | yes → yes |
| Hangul-Clock_Hangul | 1 | 4 | 205.6 → 202.9 | 198/209 → 199/209 | 80 → 80 | 2113.9 → 2168.1 | no → no |
| Inkjet__autosave-InkjetDriver | 2 | 10 | 475.3 → 471.5 | 65/65 → 64/65 | 46 → 40 | 1111.1 → 1047.6 | yes → no |
| breakout-boards_swd-and-uart | 1 | 3 | 50.7 → 50.5 | 11/11 → 11/11 | 2 → 3 | 116.6 → 117.4 | yes → yes |
| custom_cpu--ALU_custom_cpu--ALU | 2 | 8 | 472.8 → 471.8 | 71/83 → 71/83 | 20 → 19 | 1201.7 → 1200.8 | no → no |
| esp-serial-terminal_esp-com | 1 | 3 | 46.8 → 43.3 | 59/59 → 59/59 | 14 → 14 | 556.4 → 563.8 | yes → yes |
| komputer-klavier_KomputerKlavier | 2 | 15 | 163.2 → 151.0 | 87/87 → 87/87 | 2 → 2 | 728.9 → 707.1 | yes → yes |
| mosavr_pcie1x_backplane | 2 | 4 | 216.1 → 215.9 | 106/110 → 107/110 | 13 → 13 | 1287.5 → 1305.2 | no → no |
| scimpy_volumebuffer | 1 | 4 | 60.5 → 48.9 | 67/67 → 67/67 | 3 → 4 | 672.4 → 642.5 | yes → yes |
| spisolator_spisolator | 2 | 6 | 67.6 → 64.0 | 31/31 → 31/31 | 12 → 13 | 222.7 → 208.6 | yes → yes |

Totals: open connections 27 → 26 (2 boards fewer, 1 more), vias 228 → 224, length 8560 → 8509 mm.

10 boards. Median change: open connections +0.0, vias +0.0, length -1.0 mm; clean 7 → 6.
