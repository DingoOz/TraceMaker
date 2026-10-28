# Pin-swap oracle (c2-pinswap)

Runs `c2-base` (as designed) and `c2-swap` (the oracle's nets), same options and work; KiCad judges both.

| Board | Groups | Pads changed | Ratsnest in groups (mm) | Routed | Vias | Length (mm) | Clean |
|---|--:|--:|---|---|---|---|---|
| Aleste-520EX_aleste | 5 | 71 | 3215.8 → 3077.6 | 1253/1616 → 1255/1616 | 428 → 401 | 20624.6 → 20667.3 | no → no |
| CoreOne-xCORE200-Original_CoreOne | 2 | 5 | 172.9 → 170.8 | 747/807 → 747/807 | 230 → 232 | 4295.2 → 4290.5 | no → no |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 5 | 17 | 435.0 → 418.3 | 238/272 → 238/272 | 127 → 124 | 1751.2 → 1731.3 | no → no |
| Hardware_Playground_multisensor_cr2032 | 1 | 2 | 69.5 → 65.2 | 204/205 → 204/205 | 67 → 63 | 831.9 → 767.2 | no → no |
| Hardware_Playground_sd_wav_pcm5100 | 1 | 2 | 34.2 → 32.0 | 393/396 → 393/396 | 91 → 95 | 1362.2 → 1384.2 | no → no |
| Hardware_Playground_serial_gw_ATMEGA328P | 1 | 3 | 45.9 → 39.9 | 171/172 → 171/172 | 65 → 59 | 733.0 → 720.2 | no → no |
| KiCad-Library_Teensy_test_layout | 14 | 77 | 1427.0 → 1402.0 | 1238/1268 → 1233/1268 | 460 → 421 | 11342.5 → 10823.5 | no → no |
| MicroMaple_MicroMaple | 3 | 32 | 611.1 → 534.8 | 100/111 → 102/111 | 81 → 64 | 990.2 → 982.9 | no → no |
| MonApollo_analog-board | 1 | 34 | 2305.2 → 2248.3 | 1094/1190 → 1094/1190 | 572 → 578 | 9493.1 → 9557.8 | no → no |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 4 | 50 | 2000.2 → 1815.2 | 324/361 → 323/361 | 109 → 85 | 5320.9 → 4912.6 | no → no |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 1 | 3 | 105.8 → 105.8 | 153/188 → 155/188 | 43 → 49 | 1698.3 → 1813.9 | no → no |
| dorkyboard_keyboard | 1 | 2 | 46.2 → 45.3 | 810/830 → 810/830 | 123 → 125 | 11094.2 → 11093.8 | no → no |
| fifogfx_c64cart | 2 | 61 | 1958.4 → 1923.8 | 62/104 → 60/104 | 23 → 16 | 1218.2 → 1145.0 | no → no |
| kitspace_OtterPill | 2 | 19 | 224.4 → 212.9 | 122/137 → 126/137 | 71 → 73 | 539.4 → 624.6 | no → no |
| kitspace_ardfpga | 3 | 20 | 276.1 → 270.2 | 199/214 → 197/214 | 135 → 125 | 2052.0 → 1929.8 | no → no |
| kitspace_test-jig | 1 | 3 | 46.6 → 45.7 | 260/292 → 260/292 | 93 → 95 | 2084.9 → 2085.6 | no → no |
| mechkeys_58r-mxalps | 2 | 17 | 1043.2 → 928.5 | 349/361 → 349/361 | 100 → 97 | 5516.0 → 5461.9 | no → no |
| mini_mass_prog_bench_prog_rig | 19 | 57 | 1604.3 → 1588.2 | 969/995 → 969/995 | 324 → 321 | 11141.8 → 11103.0 | no → no |
| poncho_fpga_fpga-dongle | 6 | 53 | 2346.4 → 2308.9 | 333/413 → 338/413 | 161 → 162 | 3520.6 → 3663.7 | no → no |
| zx-sizif-512-ext_sizif512ext | 3 | 26 | 673.8 → 655.4 | 429/498 → 428/498 | 245 → 249 | 6569.3 → 6502.2 | no → no |

Totals: open connections 982 → 978 (5 boards fewer, 5 more), vias 3548 → 3434, length 102180 → 101261 mm.

20 boards. Median change: open connections +0.0, vias -3.0, length -10.1 mm; clean 0 → 0.
