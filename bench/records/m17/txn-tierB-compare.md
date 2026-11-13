# txn-base-tierB → txn-txn-tierB

40 boards; routed boards byte-identical: 20/40.
Clean pass 31 → 30; mean completion 97.9% → 98.1%; router seconds 3803.0 → 3506.1.

| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |
|---|---|---|---|---|---|---|---|
| avr_ledprojector_avr_ledprojection-0402 | 176 → 176 / 176 | 100.0% → 100.0% | yes → yes | – | 47.2 → 120.8 | 359.7 → 928.7 | differs |
| bumps_bumps | 330 → 330 / 330 | 100.0% → 100.0% | yes → yes | – | 120.7 → 106.8 | 775.1 → 536.7 | differs |
| fifogfx_c64cart | ? → 64 / 104 | 61.5% → 61.5% | no → no | – | 0.0 → 388.4 | – | differs |
| kitspace_BalthazarPSU3 | 204 → 204 / 204 | 100.0% → 100.0% | yes → yes | – | 20.5 → 36.7 | 96.9 → 170.2 | differs |
| kitspace_USBI2C01 | 73 → 73 / 73 | 100.0% → 100.0% | yes → yes | – | 13.3 → 16.4 | 99.1 → 125.5 | differs |
| kitspace_sympetrum-v2%20FF1.1 | 146 → 146 / 146 | 100.0% → 100.0% | yes → yes | – | 55.0 → 65.8 | 222.2 → 235.2 | differs |
| kitspace_sympetrum-v2%20NFF1.1 | 115 → 115 / 115 | 100.0% → 100.0% | yes → yes | – | 50.4 → 47.2 | 203.4 → 175.2 | differs |
| kitspace_teensy-fx | 196 → 196 / 196 | 100.0% → 100.0% | yes → yes | – | 277.7 → 277.9 | 1993.8 → 1995.2 | differs |
| kitspace_test-jig | 260 → 260 / 292 | 89.0% → 89.0% | no → no | – | 407.7 → 412.8 | 2768.3 → 2793.8 | differs |
| motor-3xdrv8833-hw_ver1 | 195 → 195 / 195 | 100.0% → 100.0% | yes → yes | – | 120.3 → 122.3 | 848.2 → 965.5 | differs |
| ourglass__autosave-watch-v4 | 134 → 134 / 134 | 100.0% → 100.0% | yes → yes | – | 120.1 → 45.5 | 412.3 → 222.2 | differs |
| poncho_fpga_fpga-dongle | 389 → 404 / 413 | 94.2% → 97.8% | no → no | – | 388.9 → 376.6 | 2689.4 → 2613.4 | differs |
| rxadc_14_rxadc_14 | 124 → 124 / 124 | 100.0% → 100.0% | yes → yes | – | 32.5 → 121.5 | 181.2 → 908.8 | differs |
| wavegen_wavegen | 140 → 140 / 140 | 100.0% → 100.0% | yes → yes | – | 69.3 → 59.2 | 410.2 → 376.6 | differs |

Style (informational): bends per connection 0.8783 → 0.8791; acute angles 2 → 0; stair steps 40 → 38; short segments 998 → 1006; off angle segments 2472 → 2425; detour ratio 1.1175 → 1.1313; long detours 191 → 187.

| Board | Bends per connection | Acute angles | Stair steps | Short segments | Off angle segments | Detour ratio | Long detours |
|---|---|---|---|---|---|---|---|
| avr_ledprojector_avr_ledprojection-0402 | 0.6532 → 0.5587 | 0 → 0 | 4 → 3 | 51 → 51 | 103 → 105 | 1.0864 → 1.0894 | 12 → 9 |
| bumps_bumps | 0.9736 → 1.046 | 0 → 0 | 5 → 5 | 147 → 151 | 87 → 95 | 1.1452 → 1.176 | 13 → 14 |
| kitspace_BalthazarPSU3 | 0.457 → 0.4393 | 0 → 0 | 1 → 0 | 47 → 42 | 84 → 81 | 1.0478 → 1.0334 | 6 → 4 |
| kitspace_USBI2C01 | 0.9748 → 1.0841 | 0 → 0 | 0 → 1 | 12 → 11 | 22 → 19 | 1.138 → 1.2224 | 4 → 8 |
| kitspace_sympetrum-v2%20FF1.1 | 0.9021 → 0.8727 | 0 → 0 | 2 → 2 | 19 → 18 | 144 → 143 | 1.1752 → 1.2286 | 11 → 13 |
| kitspace_teensy-fx | 1.048 → 1.0364 | 0 → 0 | 0 → 0 | 48 → 47 | 234 → 246 | 1.0387 → 1.0371 | 2 → 3 |
| kitspace_test-jig | 0.8209 → 0.4723 | 0 → 0 | 0 → 5 | 32 → 111 | 117 → 498 | 1.0917 → 1.0716 | 4 → 3 |
| motor-3xdrv8833-hw_ver1 | 1.2571 → 1.1688 | 0 → 0 | 3 → 1 | 48 → 45 | 74 → 87 | 1.1912 → 1.1853 | 15 → 18 |
| ourglass__autosave-watch-v4 | 0.5113 → 0.6031 | 0 → 0 | 1 → 1 | 48 → 37 | 106 → 100 | 1.0645 → 1.0611 | 3 → 4 |
| poncho_fpga_fpga-dongle | 1.4555 → 1.7398 | 0 → 0 | 4 → 1 | 86 → 85 | 200 → 238 | 1.2274 → 1.3144 | 31 → 36 |
| rxadc_14_rxadc_14 | 0.8077 → 0.7405 | 0 → 0 | 3 → 0 | 32 → 36 | 28 → 29 | 1.0609 → 1.0405 | 3 → 4 |
| wavegen_wavegen | 0.8922 → 0.8692 | 0 → 0 | 4 → 6 | 49 → 46 | 55 → 56 | 1.1474 → 1.1215 | 13 → 10 |

Gates: FAIL
- EtherCAT_shield_v1_EtherCAT_shield_v1: judge failed (after)
- Hardware_Playground_E3_Pcb_rev0.3: judge failed (before)
- Hardware_Playground_serial_gw_ATMEGA328P: judge failed (after)
- MicroMaple_MicroMaple: judge failed (before)
- kitspace_OtterPill: judge failed (before)
- kitspace_ardfpga: judge failed (after)
- kitspace_test-jig: before evaluated DRC errors are capped or unavailable; gate unknown
- kitspace_test-jig: after evaluated DRC errors are capped or unavailable; gate unknown
- poncho_fpga_fpga-dongle: before evaluated DRC errors are capped or unavailable; gate unknown
- poncho_fpga_fpga-dongle: after evaluated DRC errors are capped or unavailable; gate unknown
