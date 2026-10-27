# Open connections by cause (c5-causes)

Run `c3-bd-base`; each open connection routed again alone at 2,000,000 work with other nets' copper removed near its ends (within 2.0 mm of an end pad) or away from them; the board as it is and a copy with 2 more inner layers routed at 10,000,000 work.

| Board | Layers | Open | Routes as is | pin_cannot_escape | through_traffic | package_fanout | channel | near_and_far | no_path_bare | Open with +2 layers |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 96boards-robomezzi_96boards-robomezzi | 4 | 2 | 0 | 0 | 0 | 2 | 0 | 0 | 0 | 2 |
| Aleste-520EX_aleste | 2 | 363 | 8 | 0 | 145 | 36 | 139 | 11 | 24 | 287 |
| CoreOne-xCORE200-Original_CoreOne | 4 | 60 | 2 | 0 | 31 | 24 | 1 | 2 | 0 | 58 |
| DronPi_dronPi | 2 | 11 | 0 | 0 | 0 | 0 | 0 | 0 | 11 | 11 |
| ErgodoneWithHole_ErgoDone | 2 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 2 | 0 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 2 | 34 | 4 | 0 | 4 | 26 | 0 | 0 | 0 | 30 |
| Hardware_Playground_E3_Pcb_rev0.3 | 2 | 26 | 0 | 0 | 3 | 8 | 0 | 0 | 15 | 14 |
| Hardware_Playground_multisensor_cr2032 | 2 | 1 | 0 | 0 | 0 | 0 | 1 | 0 | 0 | 0 |
| Hardware_Playground_sd_wav_pcm5100 | 2 | 3 | 0 | 0 | 0 | 3 | 0 | 0 | 0 | 3 |
| Hardware_Playground_serial_gw_ATMEGA328P | 2 | 1 | 0 | 0 | 0 | 1 | 0 | 0 | 0 | 1 |
| KiCad-Library_Teensy_test_layout | 2 | 30 | 6 | 0 | 2 | 8 | 7 | 5 | 2 | 21 |
| LeeChee_1800 | 2 | 46 | 1 | 0 | 1 | 11 | 18 | 7 | 8 | 46 |
| MicroMaple_MicroMaple | 2 | 11 | 1 | 0 | 1 | 9 | 0 | 0 | 0 | 7 |
| MonApollo_analog-board | 2 | 96 | 2 | 0 | 26 | 41 | 20 | 7 | 0 | 91 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 2 | 37 | 2 | 0 | 3 | 4 | 15 | 3 | 10 | 24 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 2 | 35 | 0 | 30 | 0 | 5 | 0 | 0 | 0 | 35 |
| TCKB_Kicad_TCKB | 2 | 43 | 5 | 0 | 3 | 5 | 1 | 1 | 28 | 38 |
| dorkyboard_keyboard | 2 | 20 | 4 | 0 | 0 | 8 | 2 | 6 | 0 | 15 |
| fifogfx_c64cart | 2 | 42 | 0 | 0 | 1 | 1 | 0 | 0 | 40 | 40 |
| front-end-modules_LimeSDR_Sony | 6 | 8 | 3 | 0 | 1 | 4 | 0 | 0 | 0 | 6 |
| kitspace_OtterPill | 2 | 15 | 1 | 0 | 1 | 12 | 0 | 0 | 1 | 12 |
| kitspace_ardfpga | 2 | 15 | 0 | 0 | 6 | 9 | 0 | 0 | 0 | 15 |
| kitspace_d20_tri_r0.3 | 4 | 150 | 3 | 0 | 109 | 38 | 0 | 0 | 0 | 148 |
| kitspace_teensy-fx | 4 | 22 | 0 | 0 | 12 | 10 | 0 | 0 | 0 | 22 |
| kitspace_test-jig | 2 | 32 | 0 | 0 | 0 | 0 | 0 | 0 | 32 | 32 |
| mechkeys_58r-mxalps | 2 | 12 | 0 | 0 | 0 | 0 | 10 | 2 | 0 | 5 |
| mechkeys_LFK78 | 2 | 45 | 6 | 0 | 15 | 9 | 12 | 3 | 0 | 28 |
| memsarray_mems_modules | 2 | 1 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 1 |
| mini_mass_prog_bench_prog_rig | 4 | 26 | 1 | 0 | 10 | 11 | 0 | 3 | 1 | 25 |
| motor-high-current-sensors_CurrentSensorPCB_line | 2 | 17 | 6 | 2 | 1 | 0 | 0 | 0 | 8 | 11 |
| poncho_fpga_fpga-dongle | 2 | 80 | 2 | 0 | 45 | 26 | 7 | 0 | 0 | 80 |
| rvi_v2x_hardware_V2X | 2 | 28 | 1 | 0 | 10 | 11 | 6 | 0 | 0 | 27 |
| zx-sizif-512-ext_sizif512ext | 2 | 69 | 5 | 16 | 14 | 13 | 20 | 1 | 0 | 61 |

**Total** 1383 open in the run; 1320 still open on the board as it is.

| Cause | Open | At a dense package |
|---|--:|--:|
| pin_cannot_escape | 49 | 49 |
| through_traffic | 444 | 204 |
| package_fanout | 335 | 264 |
| channel | 259 | 69 |
| near_and_far | 51 | 34 |
| no_path_bare | 182 | 64 |

With 2 more inner layers: 1320 → 1196 open on 33 boards (2 complete).
