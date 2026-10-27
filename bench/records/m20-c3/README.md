# M20 C3: generated pours and the layer-count advisor (2026-10-27)

Design: doc 05 §53, decision D138, doc 18 §10. Binary: branch `test-campaign-fixes` with the C3 changes, release build,
no GPU (`--no-gpu`; the `c3-layers` check was run without that option and may have used one, which changes speed, not results), 10 M work units per board, `--time 3600`, 8 threads, 3 or 4 boards at a time on the shared server.
KiCad 10.0.6 judges every board with `--refill-zones` (`bench/run.py --refill`).

## Runs

| Run | What | Set |
|---|---|---|
| `runs/c3-bd-base` | without a pour | `pcbench-bd-fail` (33 boards) |
| `runs/c3-bd-pour` | `--pour auto`, thermal reliefs (KiCad's default; the first version) | same |
| `runs/c3-bd-pour2` | `--pour auto`, solid pad connections (as built) | same |
| `runs/c3-layers-L{2,4,6}`, `runs/c3-layers` | brute force at 2, 4, 6 layers and the advisor, no pour | `pcbench-c3-layers.txt` (14 boards) |
| `runs/c3-layers-pour-L{2,4,6}`, `runs/c3-layers-pour` | the same with `--pour auto` | same |
| `runs/c3-quick`, `quick-compare.txt` | quick tier, options off, against `fix-g-quick` | `quick` (30 boards) |

Commands:

```
bench/run.py --set pcbench-bd-fail --work 10000000 --refill --jobs 3 --threads 8 --view --name c3-bd-base --route-args=--no-gpu
bench/run.py --set pcbench-bd-fail --work 10000000 --refill --jobs 3 --threads 8 --view --name c3-bd-pour2 --route-args="--no-gpu --pour auto"
bench/layer_advisor_check.py --set pcbench-c3-layers --counts 2,4,6 --work 10000000 --name c3-layers --jobs 3 --threads 8 --view
bench/layer_advisor_check.py --set pcbench-c3-layers --counts 2,4,6 --work 10000000 --name c3-layers-pour --jobs 4 --threads 8 --view --route-args "--no-gpu --pour auto"
bench/run.py --set quick --work 1000000 --jobs 2 --threads 4 --view --name c3-quick
```

The advisor's `route.json` files are kept without the viewer options (they name the machine's addresses).

## Pours on the 33 boards

| Run | Clean boards | Open after refill (sum) | Better / worse than without | Added errors | Wall time |
|---|--:|--:|---|---|--:|
| Without a pour | 0 | 1,368 | | none | 1,267 s |
| Thermal reliefs | 0 | 1,297 | 20 / 8 | `starved_thermal` on 22 boards (2 to 18), `solder_mask_bridge` 2 | 11,240 s |
| Solid connections | 0 | 1,290 | 18 / 10 | `solder_mask_bridge` 2 (Teensy test layout) | 3,905 s |

Aleste took 3,708 s with reliefs: about 200 s in the refill repair and about 3,400 s in D137's thermal repair, which routes
again pad by pad with the full budget each (timed run, `--view`, same options). With solid connections it took 457 s.

| Board | Without: routed / open after refill | Reliefs: routed / open / added | Solid: routed / open / added | Seconds (without / reliefs / solid) |
|---|--:|--:|--:|--:|
| Hardware_Playground_serial_gw_ATMEGA328P | 171/172 / 1 | 78/78 / 1 / starved_thermal 3 | 78/78 / 2 / none | 17 / 47 / 31 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 238/272 / 33 | 207/240 / 32 / starved_thermal 7 | 207/240 / 32 / none | 23 / 170 / 47 |
| Hardware_Playground_E3_Pcb_rev0.3 | 315/341 / 13 | 292/326 / 22 / none | 292/326 / 22 / none | 29 / 47 / 47 |
| fifogfx_c64cart | 62/104 / 42 | 42/82 / 40 / starved_thermal 2 | 42/82 / 40 / none | 16 / 50 / 41 |
| MicroMaple_MicroMaple | 100/111 / 11 | 101/110 / 9 / none | 101/110 / 9 / none | 35 / 55 / 45 |
| kitspace_OtterPill | 122/137 / 15 | 119/130 / 14 / starved_thermal 4 | 121/130 / 11 / none | 24 / 37 / 29 |
| kitspace_ardfpga | 199/214 / 15 | 196/210 / 14 / none | 196/210 / 14 / none | 21 / 39 / 38 |
| memsarray_mems_modules | 9/10 / 1 | 9/10 / 1 / starved_thermal 1 | 9/10 / 1 / none | 8 / 13 / 13 |
| kitspace_test-jig | 260/292 / 32 | 246/276 / 31 / starved_thermal 2 | 246/276 / 31 / none | 20 / 65 / 37 |
| kitspace_teensy-fx | 174/196 / 22 | 156/177 / 21 / none | 156/177 / 21 / none | 28 / 43 / 50 |
| poncho_fpga_fpga-dongle | 333/413 / 80 | 291/354 / 65 / starved_thermal 4 | 299/354 / 55 / none | 24 / 127 / 90 |
| DronPi_dronPi | 341/352 / 11 | 312/322 / 12 / none | 312/322 / 12 / none | 25 / 51 / 73 |
| ErgodoneWithHole_ErgoDone | 354/356 / 2 | 340/342 / 2 / none | 340/342 / 2 / none | 26 / 169 / 219 |
| Hardware_Playground_multisensor_cr2032 | 204/205 / 1 | 93/93 / 0 / starved_thermal 1 | 92/93 / 2 / none | 27 / 45 / 62 |
| 96boards-robomezzi_96boards-robomezzi | 280/282 / 2 | 243/245 / 1 / none | 243/245 / 1 / none | 66 / 91 / 142 |
| Hardware_Playground_sd_wav_pcm5100 | 393/396 / 3 | 111/113 / 3 / none | 111/113 / 3 / none | 21 / 22 / 30 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 153/188 / 35 | 139/167 / 28 / starved_thermal 4 | 139/167 / 28 / none | 31 / 49 / 55 |
| kitspace_d20_tri_r0.3 | 403/553 / 150 | 403/553 / 150 / none | 403/553 / 150 / none | 31 / 28 / 35 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 324/361 / 37 | 134/172 / 42 / starved_thermal 3 | 134/172 / 41 / none | 55 / 226 / 124 |
| mechkeys_58r-mxalps | 349/361 / 12 | 229/241 / 14 / starved_thermal 4 | 229/241 / 14 / none | 34 / 284 / 116 |
| motor-high-current-sensors_CurrentSensorPCB_line | 455/472 / 16 | 366/376 / 13 / starved_thermal 18 | 364/374 / 13 / none | 38 / 575 / 81 |
| rvi_v2x_hardware_V2X | 451/479 / 28 | 367/391 / 34 / starved_thermal 15 | 368/391 / 35 / none | 35 / 478 / 80 |
| zx-sizif-512-ext_sizif512ext | 429/498 / 69 | 351/416 / 66 / starved_thermal 12 | 351/416 / 67 / none | 23 / 728 / 90 |
| Aleste-520EX_aleste | 1253/1616 / 363 | 1040/1382 / 341 / starved_thermal 12 | 1040/1382 / 341 / none | 28 / 3708 / 457 |
| CoreOne-xCORE200-Original_CoreOne | 747/807 / 60 | 681/737 / 54 / none | 681/737 / 54 / none | 33 / 54 / 65 |
| KiCad-Library_Teensy_test_layout | 1238/1268 / 30 | 1034/1070 / 48 / solder_mask_bridge 2, starved_thermal 2 | 1034/1070 / 48 / solder_mask_bridge 2 | 29 / 544 / 184 |
| TCKB_Kicad_TCKB | 554/597 / 43 | 444/480 / 36 / starved_thermal 1 | 440/480 / 40 / none | 24 / 275 / 177 |
| MonApollo_analog-board | 1094/1190 / 96 | 1068/1119 / 55 / starved_thermal 4 | 1068/1119 / 55 / none | 38 / 505 / 145 |
| LeeChee_1800 | 1114/1160 / 46 | 1112/1158 / 45 / none | 1112/1158 / 46 / none | 52 / 234 / 252 |
| mechkeys_LFK78 | 848/893 / 45 | 544/579 / 51 / starved_thermal 4 | 544/579 / 51 / none | 38 / 1287 / 265 |
| front-end-modules_LimeSDR_Sony | 806/814 / 8 | 612/618 / 4 / starved_thermal 1 | 612/618 / 4 / none | 76 / 126 / 132 |
| mini_mass_prog_bench_prog_rig | 969/995 / 26 | 874/908 / 32 / none | 874/908 / 32 / none | 48 / 82 / 84 |
| dorkyboard_keyboard | 810/830 / 20 | 594/606 / 16 / starved_thermal 2 | 594/605 / 13 / none | 245 / 984 / 569 |

## The advisor against a brute force (14 two-layer boards)

| 14 boards, KiCad's count | 2 layers | 4 layers | 6 layers |
|---|--:|--:|--:|
| Clean without a pour | 0 | 1 (MicroMaple) | 1 (MicroMaple) |
| Open after refill without a pour | 293 | 296 | 329 |
| Clean with `--pour auto` | 0 | 1 (serial_gw) | 2 (serial_gw, MicroMaple) |
| Open after refill with `--pour auto` | 262 | 286 | 322 |

Answers match on 14 of 14 boards in both checks (`runs/c3-layers/layer_check.md`, `runs/c3-layers-pour/layer_check.md`).
Without a pour the advisor writes MicroMaple at 4 layers, byte-identical to the brute force's 4-layer board; with
`--pour auto` it writes serial_gw at 4 and MicroMaple at 6.

## Gate (doc 18 C3)

- Boards completed with a generated pour against without, DRC clean: 0 against 0 on the 33 boards at their own layer
  counts; on the 14 boards 0 / 0 at 2 layers, 1 / 1 at 4, 2 / 1 at 6. No routing error added except the two mask bridges.
- The advisor's answer equals the brute force's: 28 of 28. Met.

## Tests

`tm_tests "[pour]"` (6 cases, 70 assertions) passes in the release and ASan builds. The full release `ctest` (400
tests) passed except one run of `legalisation with subdivision: a straight track between fixed ends bends away from a
pinch` while the benchmarks loaded the machine; it passed on its own and 16 of 16 times in parallel, and no routing code
changed. Quick tier with the options off: 30/30 boards byte-identical (`quick-compare.txt`).
