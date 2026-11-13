# M22 item 4: informed pad-to-plane searches on the gate set (D179)

2026-11-13, branch `roadmap-2026-11-13` on main `816634e`. `run.sh`: `bench/run.py --set pcbench-bd-fail --work 10000000 --time 7200
--jobs 7 --threads 4`, base and `--plane-informed` (the octile bound to the plane's box, now plus one via for a state off the plane's
layer, and the late reachability proof forward to the plane; the plane fallback), the same binary. 33 boards, 8 variants.

**Result: the 33 routed boards are byte-identical** (`cmp` of every output `.kicad_pcb`). Routed 15,858 in both arms; KiCad
unconnected 1,061 in the base. One judge run of the plane arm failed (`Hardware_Playground_E3_Pcb_rev0.3`: `kicad-cli` exited with
an error; its routed board has the same bytes as the base's). On these boards no pad-to-plane search reaches the point where the
bound, the proof or the fallback changes a decision at 10 M work, so item 4 does nothing for the M22 gate.

`causes-poe-*.json`: `tracemaker causes --baseline-only --baseline-work 3000000` on WholeBoard's PoE switch (placed, 21 zones, 2 copper
zones without a fill): with the new default the 2 zones are filled in memory and routed soft, as `route` does (100 routed, 1,089 open);
with `--no-fill-planes`, as before (90 routed, 1,104 open).

## Base, per board

| Board | Routed | KiCad unconnected | Clean |
|---|--:|--:|:-:|
| `96boards-robomezzi_96boards-robomezzi` | 280/282 | 2 |  |
| `Aleste-520EX_aleste` | 1277/1616 | 339 |  |
| `CoreOne-xCORE200-Original_CoreOne` | 788/807 | 19 |  |
| `DronPi_dronPi` | 341/352 | 11 |  |
| `ErgodoneWithHole_ErgoDone` | 353/356 | 3 |  |
| `EtherCAT_shield_v1_EtherCAT_shield_v1` | 246/272 | 25 |  |
| `Hardware_Playground_E3_Pcb_rev0.3` | 312/341 | 16 |  |
| `Hardware_Playground_multisensor_cr2032` | 205/205 | 0 | yes |
| `Hardware_Playground_sd_wav_pcm5100` | 393/396 | 3 |  |
| `Hardware_Playground_serial_gw_ATMEGA328P` | 171/172 | 1 |  |
| `KiCad-Library_Teensy_test_layout` | 1246/1268 | 22 |  |
| `LeeChee_1800` | 1139/1160 | 21 |  |
| `MicroMaple_MicroMaple` | 102/111 | 9 |  |
| `MonApollo_analog-board` | 1136/1190 | 54 |  |
| `P8000_WDC_Emulator_P8000_WDC_Emulator` | 329/361 | 32 |  |
| `PCIE-to-MXM-Adapter_PCIEx1toMXM3.0` | 158/188 | 30 |  |
| `TCKB_Kicad_TCKB` | 576/597 | 21 |  |
| `dorkyboard_keyboard` | 821/830 | 9 |  |
| `fifogfx_c64cart` | 64/104 | 40 |  |
| `front-end-modules_LimeSDR_Sony` | 812/814 | 2 |  |
| `kitspace_OtterPill` | 124/137 | 13 |  |
| `kitspace_ardfpga` | 207/214 | 7 |  |
| `kitspace_d20_tri_r0.3` | 404/553 | 149 |  |
| `kitspace_teensy-fx` | 191/196 | 5 |  |
| `kitspace_test-jig` | 260/292 | 32 |  |
| `mechkeys_58r-mxalps` | 355/361 | 6 |  |
| `mechkeys_LFK78` | 847/893 | 46 |  |
| `memsarray_mems_modules` | 9/10 | 1 |  |
| `mini_mass_prog_bench_prog_rig` | 985/995 | 10 |  |
| `motor-high-current-sensors_CurrentSensorPCB_line` | 466/472 | 4 |  |
| `poncho_fpga_fpga-dongle` | 376/413 | 37 |  |
| `rvi_v2x_hardware_V2X` | 455/479 | 24 |  |
| `zx-sizif-512-ext_sizif512ext` | 430/498 | 68 |  |
