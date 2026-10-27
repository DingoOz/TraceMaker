# Where the work went (run `c5-base`, 10 M work units per board)

From each board's `route.json` (`work`: first pass and negotiation, expansions plus flood cells) and `failures` (whether the
router's reason for an open connection includes a negotiated attempt).

| Board | Connections | Open | First pass | Negotiation | Open connections tried in negotiation |
|---|--:|--:|--:|--:|--:|
| 96boards-robomezzi_96boards-robomezzi | 282 | 2 | 3.6 M | 6.4 M | 0 |
| Aleste-520EX_aleste | 1616 | 363 | 10.0 M | 0.0 M | 0 |
| CoreOne-xCORE200-Original_CoreOne | 807 | 60 | 10.1 M | 0.0 M | 0 |
| DronPi_dronPi | 352 | 11 | 6.6 M | 3.4 M | 11 |
| ErgodoneWithHole_ErgoDone | 356 | 2 | 4.7 M | 5.3 M | 2 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 272 | 34 | 10.3 M | 0.0 M | 0 |
| Hardware_Playground_E3_Pcb_rev0.3 | 341 | 26 | 9.3 M | 0.8 M | 15 |
| Hardware_Playground_multisensor_cr2032 | 205 | 1 | 1.6 M | 8.4 M | 0 |
| Hardware_Playground_sd_wav_pcm5100 | 396 | 3 | 1.6 M | 8.4 M | 0 |
| Hardware_Playground_serial_gw_ATMEGA328P | 172 | 1 | 3.2 M | 6.8 M | 0 |
| KiCad-Library_Teensy_test_layout | 1268 | 30 | 10.1 M | 0.0 M | 0 |
| LeeChee_1800 | 1160 | 46 | 10.7 M | 0.0 M | 0 |
| MicroMaple_MicroMaple | 111 | 11 | 3.9 M | 6.2 M | 0 |
| MonApollo_analog-board | 1190 | 96 | 10.1 M | 0.0 M | 0 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 361 | 37 | 10.1 M | 0.0 M | 0 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 188 | 35 | 7.3 M | 3.8 M | 0 |
| TCKB_Kicad_TCKB | 597 | 43 | 10.1 M | 0.0 M | 0 |
| dorkyboard_keyboard | 830 | 20 | 3.2 M | 7.1 M | 0 |
| fifogfx_c64cart | 104 | 42 | 5.0 M | 5.1 M | 39 |
| front-end-modules_LimeSDR_Sony | 814 | 8 | 4.7 M | 5.4 M | 0 |
| kitspace_OtterPill | 137 | 15 | 5.4 M | 4.8 M | 0 |
| kitspace_ardfpga | 214 | 15 | 10.1 M | 0.0 M | 0 |
| kitspace_d20_tri_r0.3 | 553 | 150 | 10.0 M | 0.0 M | 0 |
| kitspace_teensy-fx | 196 | 22 | 11.4 M | 0.0 M | 0 |
| kitspace_test-jig | 292 | 32 | 6.8 M | 3.3 M | 0 |
| mechkeys_58r-mxalps | 361 | 12 | 10.5 M | 0.0 M | 0 |
| mechkeys_LFK78 | 893 | 45 | 10.1 M | 0.0 M | 0 |
| memsarray_mems_modules | 10 | 1 | 6.8 M | 3.2 M | 1 |
| mini_mass_prog_bench_prog_rig | 995 | 26 | 12.1 M | 0.0 M | 0 |
| motor-high-current-sensors_CurrentSensorPCB_line | 472 | 17 | 6.1 M | 4.2 M | 11 |
| poncho_fpga_fpga-dongle | 413 | 80 | 10.1 M | 0.0 M | 0 |
| rvi_v2x_hardware_V2X | 479 | 28 | 10.4 M | 0.0 M | 0 |
| zx-sizif-512-ext_sizif512ext | 498 | 69 | 10.3 M | 0.0 M | 0 |

17 of 33 boards spend the whole budget in the strict first pass and never negotiate; they hold 1156 of the 1383 open
connections. 79 open connections in all were tried in negotiation.
