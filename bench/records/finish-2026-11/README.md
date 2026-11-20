# Finishing boards that never complete: repair, a ladder on top, pad masks (2026-11-19)

Doc 20 §2.11–§2.13, D195–D197. The 33 failing PCBench boards of `bench/sets/pcbench-bd-fail.txt`, 8 variants, 8 threads, seed 1, `--no-kb --no-gpu`.
Follows `bench/records/oracle-2026-11` (rule fit, chain measurement); the arms `base`, `fit3` and `fitref3` are that record's.

## Summary

- Repair windows after a 20 M run close little: 887 open → 859 (windows round the ends), 857 (along the bare-board route, 39 % less work),
  840 (both). No board completes.
- Routing **only what is open, on top of the default-rules result**, with the rules fitted to the board: 890 → 597 open, 7 boards complete
  (572 and 8 fitted from the designer's layout). Fitted from nothing it was 771 and 6. On top it never ends worse than it began.
- Extra copper layers as later rungs: 10 complete at +4. Aleste-520EX goes 250 → 195 → 8 → 0, P8000 17 → 0 at +2, MicroMaple 0 at +4.
- Three times the base budget with the default rules (`ctl60`): 731 open, 6 complete. It finishes boards the ladder leaves one or two short
  (serial_gw, teensy-fx, LeeChee) and CoreOne goes 19 → 2, so budget and ladder add up: the ladder on top of the 60 M result was started and
  completed ardfpga for the first time before the runs were stopped (below).
- Nine designer layouts run tracks through other nets' pad solder-mask openings (KiCad reports 3 to 118 `solder_mask_bridge` on them). Fitted
  from the designer's layout (D197), memsarray completes for the first time, sizif goes 38 → 26, LFK78 31 → 27.
- `route --finish --finish-masks --finish-layers 2,4`, one command per board, 29 of the 30 boards open at base: **881 → 219 open, 8 complete**
  (127 of the 219 are d20).
- Best arm per board, no designer layout needed: **890 → 245 open, 16 of 33 complete** (3 before); the stronger ladder (60 M, the
  from-nothing rung, eight repair variants) then completed DronPi, Teensy_test_layout and dorkyboard: **19 of 33**. With the designer's layout as reference:
  233 open, 17 complete. 129 of the 245 are d20.
- KiCad 10.0.6 on the newly complete outputs: 0 unconnected on 12 of 13 checked (MicroMaple's `toplay4` output has 1 unconnected item; its
  `c60lay2` output has none), no shorts; added violations only of the kinds that were relaxed (`ontop/kicad-judge.json`).

**Not finished.** At 15:05 UTC the session's background runs were stopped for low memory (too many jobs at once; my scheduling, not a fault of
the router). Missing: the ladder on the 60 M result for 19 boards, the seed spread for 5 of 8 boards, the focused repair on d20, `topmask` for
4 boards, `toplay4` for mini_mass. The scripts skip what is done: run them again to fill the gaps (`scripts/`), fewer jobs at a time.

## Arms

| Arm | Input | Options |
|---|---|---|
| base | unrouted | 20 M work |
| ctl60 | unrouted | 60 M work |
| fit3 / fitref3 | unrouted | 20 M, `--fit-rules` / `--fit-rules-from` designer's layout |
| ends / route / both | unrouted | 20 M, `--repair on` with `--repair-select` ends, route, both |
| topfit / topfitref | base's output | 20 M, fit (own pads / designer's layout), `--repair on --repair-select both` |
| topmask | topfit's output | as topfit with `--soft-pad-masks` |
| topmaskref | topfitref's output | as topfitref, with D197 (pad masks soft where the designer's tracks cross them) |
| toplay2 / toplay4 | topfit's / toplay2's output | as topfit with 2 / 4 copper layers more than the board has |
| toprv8 | topfit's output | as topfit with `--repair-variants 8` |
| topfocus | toplay2's output | as topfit with `--repair-focus --repair-conns 300` |
| c60fit, c60mask, c60lay2, c60lay4 | ctl60's output, then each the one before | the ladder with `--repair-focus`; masks soft from c60mask on |
| fitseedN | unrouted | fit3 at seed N |

A rung that had nothing open is not run; the tables carry its 0 forward.

## Repair after a 20 M run (`repair/results.jsonl`)

| Arm | Open before | Open after | Closed | Repair work (expansions) | Wall time, all boards |
|---|--:|--:|--:|--:|--:|
| ends (D172) | 887 | 859 | 28 | 4.90 G | 9,290 s |
| route (D195) | 887 | 857 | 30 | 2.99 G | 7,606 s |
| both | 887 | 840 | 47 | 7.50 G | 12,164 s |

"Open" here is the job's final count (after the zone refill where a board has soft zones), so it differs by 3 from the 890 of the base run's
table. 169 open connections had no route with the other nets' copper off the board; the route selection spends one run on each and leaves them
(fifogfx: 13 M of work against 299 M for the end windows, both closing nothing).

## The ladder (open connections)

| Board | base | ctl60 | fit3 | topfit | topfitref | topmask | topmaskref | toplay2 | toplay4 |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 96boards-robomezzi_96boards-robomezzi | 2 | 2 | 2 | 1 | 1 | 1 | – | 1 | 1 |
| Aleste-520EX_aleste | 250 | 185 | 250 | 195 | 195 | – | – | 8 | 0 |
| CoreOne-xCORE200-Original_CoreOne | 19 | 2 | 19 | 14 | 14 | 14 | – | 14 | 14 |
| DronPi_dronPi | 11 | 11 | 9 | 9 | 6 | 2 | – | 9 | 9 |
| ErgodoneWithHole_ErgoDone | 2 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 26 | 26 | 24 | 20 | 20 | 18 | – | 20 | 20 |
| Hardware_Playground_E3_Pcb_rev0.3 | 19 | 19 | 19 | 18 | 15 | 17 | – | 4 | 4 |
| Hardware_Playground_multisensor_cr2032 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| Hardware_Playground_sd_wav_pcm5100 | 2 | 2 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| Hardware_Playground_serial_gw_ATMEGA328P | 1 | 0 | 1 | 1 | 1 | 1 | – | 1 | 1 |
| KiCad-Library_Teensy_test_layout | 18 | 16 | 2 | 3 | 3 | 3 | – | 2 | 2 |
| LeeChee_1800 | 5 | 0 | 6 | 1 | 0 | 1 | 0 | 1 | 1 |
| MicroMaple_MicroMaple | 9 | 6 | 9 | 8 | 8 | 7 | – | 2 | 0 |
| MonApollo_analog-board | 44 | 39 | 45 | 35 | 33 | – | – | 18 | 16 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 25 | 17 | 25 | 17 | 17 | 16 | – | 0 | 0 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 30 | 30 | 0 | 2 | 1 | 2 | – | 1 | 1 |
| TCKB_Kicad_TCKB | 22 | 21 | 12 | 8 | 5 | 4 | – | 2 | 2 |
| dorkyboard_keyboard | 3 | 2 | 3 | 1 | 1 | 1 | 1 | 1 | 1 |
| fifogfx_c64cart | 40 | 40 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| front-end-modules_LimeSDR_Sony | 0 | 0 | 1 | 0 | 0 | 0 | 0 | 0 | 0 |
| kitspace_OtterPill | 11 | 8 | 11 | 9 | 3 | 9 | – | 4 | 4 |
| kitspace_ardfpga | 7 | 3 | 7 | 6 | 6 | 6 | – | 3 | 3 |
| kitspace_d20_tri_r0.3 | 142 | 136 | 142 | 129 | 123 | 129 | – | 129 | 129 |
| kitspace_teensy-fx | 2 | 0 | 2 | 2 | 2 | 2 | – | 2 | 2 |
| kitspace_test-jig | 32 | 32 | 0 | 0 | 0 | 0 | 0 | 0 | 0 |
| mechkeys_58r-mxalps | 0 | 0 | 5 | 0 | 0 | 0 | 0 | 0 | 0 |
| mechkeys_LFK78 | 45 | 32 | 45 | 31 | 31 | – | 27 | 14 | 12 |
| memsarray_mems_modules | 1 | 1 | 1 | 1 | 1 | 0 | 0 | 1 | 1 |
| mini_mass_prog_bench_prog_rig | 9 | 9 | 9 | 7 | 7 | – | – | 7 | – |
| motor-high-current-sensors_CurrentSensorPCB_line | 5 | 5 | 5 | 3 | 3 | 3 | – | 3 | 3 |
| poncho_fpga_fpga-dongle | 33 | 32 | 45 | 27 | 27 | 27 | 27 | 17 | 16 |
| rvi_v2x_hardware_V2X | 21 | 9 | 18 | 11 | 11 | 8 | – | 2 | 2 |
| zx-sizif-512-ext_sizif512ext | 54 | 44 | 54 | 38 | 38 | 26 | 26 | 30 | 30 |
| **open** | 890 | 731 | 771 | 597 | 572 | 297 | 81 | 296 | 274 |
| **complete** | 3 | 6 | 6 | 7 | 8 | 8 | 9 | 8 | 10 |
| boards run | 33 | 33 | 33 | 33 | 33 | 29 | 13 | 33 | 32 |

## The ladder on the 60 M result (stopped early)

| Board | ctl60 | c60fit | c60mask | c60lay2 | c60lay4 |
|---|--:|--:|--:|--:|--:|
| 96boards-robomezzi_96boards-robomezzi | 2 | 1 | 1 | – | – |
| Aleste-520EX_aleste | 185 | – | – | – | – |
| CoreOne-xCORE200-Original_CoreOne | 2 | – | – | – | – |
| DronPi_dronPi | 11 | – | – | – | – |
| ErgodoneWithHole_ErgoDone | 2 | – | – | – | – |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 26 | – | – | – | – |
| Hardware_Playground_E3_Pcb_rev0.3 | 19 | 16 | – | – | – |
| Hardware_Playground_multisensor_cr2032 | 0 | 0 | 0 | 0 | 0 |
| Hardware_Playground_sd_wav_pcm5100 | 2 | – | – | – | – |
| Hardware_Playground_serial_gw_ATMEGA328P | 0 | 0 | 0 | 0 | 0 |
| KiCad-Library_Teensy_test_layout | 16 | – | – | – | – |
| LeeChee_1800 | 0 | 0 | 0 | 0 | 0 |
| MicroMaple_MicroMaple | 6 | 4 | 4 | 0 | 0 |
| MonApollo_analog-board | 39 | – | – | – | – |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 17 | – | – | – | – |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 30 | – | – | – | – |
| TCKB_Kicad_TCKB | 21 | – | – | – | – |
| dorkyboard_keyboard | 2 | – | – | – | – |
| fifogfx_c64cart | 40 | 0 | 0 | 0 | 0 |
| front-end-modules_LimeSDR_Sony | 0 | 0 | 0 | 0 | 0 |
| kitspace_OtterPill | 8 | 5 | 5 | 2 | 2 |
| kitspace_ardfpga | 3 | 0 | 0 | 0 | 0 |
| kitspace_d20_tri_r0.3 | 136 | – | – | – | – |
| kitspace_teensy-fx | 0 | 0 | 0 | 0 | 0 |
| kitspace_test-jig | 32 | 0 | 0 | 0 | 0 |
| mechkeys_58r-mxalps | 0 | 0 | 0 | 0 | 0 |
| mechkeys_LFK78 | 32 | – | – | – | – |
| memsarray_mems_modules | 1 | 1 | 0 | 0 | 0 |
| mini_mass_prog_bench_prog_rig | 9 | – | – | – | – |
| motor-high-current-sensors_CurrentSensorPCB_line | 5 | – | – | – | – |
| poncho_fpga_fpga-dongle | 32 | – | – | – | – |
| rvi_v2x_hardware_V2X | 9 | – | – | – | – |
| zx-sizif-512-ext_sizif512ext | 44 | – | – | – | – |
| **open** | 731 | 27 | 10 | 2 | 2 |
| **complete** | 6 | 9 | 10 | 11 | 11 |
| boards run | 33 | 14 | 13 | 12 | 12 |

## `route --finish` end to end (`finish/results.jsonl`)

One command per board: `route --work 20000000 --finish --finish-masks --finish-layers 2,4`, on the 30 boards open at base: 24 three at a
time, then the six largest two at a time (`scripts/finishrun.py`, `finishrun2.py`). Open connections after each rung; (x) = rung discarded,
no better.

| Board | base | fitted rules | + pad masks | + 2 layers | + 4 layers | final | layers | seconds |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| memsarray_mems_modules | 1 | 1 (x) | 0 | – | – | 0 | 2 | 26 |
| Hardware_Playground_sd_wav_pcm5100 | 2 | 0 | – | – | – | 0 | 2 | 86 |
| Hardware_Playground_serial_gw_ATMEGA328P | 1 | 1 (x) | 1 (x) | 1 (x) | 1 (x) | 1 | 2 | 116 |
| fifogfx_c64cart | 40 | 0 | – | – | – | 0 | 2 | 91 |
| kitspace_ardfpga | 7 | 6 | 6 (x) | 3 | 3 (x) | 3 | 4 | 525 |
| kitspace_OtterPill | 11 | 9 | 9 (x) | 4 | 3 | 3 | 6 | 487 |
| Hardware_Playground_E3_Pcb_rev0.3 | 19 | 17 | 17 (x) | 4 | 4 (x) | 4 | 4 | 509 |
| MicroMaple_MicroMaple | 9 | 8 | 7 | 0 | – | 0 | 4 | 488 |
| ErgodoneWithHole_ErgoDone | 2 | 0 | – | – | – | 0 | 2 | 90 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 26 | 17 | 16 | 16 (x) | 16 (x) | 16 | 2 | 1270 |
| kitspace_test-jig | 32 | 0 | – | – | – | 0 | 2 | 112 |
| DronPi_dronPi | 11 | 9 | 2 | 1 | 1 (x) | 1 | 4 | 326 |
| motor-high-current-sensors_CurrentSensorPCB_line | 5 | 3 | 3 (x) | 3 (x) | 3 (x) | 3 | 2 | 204 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 30 | 2 | 2 (x) | 1 | 1 (x) | 1 | 4 | 393 |
| kitspace_teensy-fx | 2 | 2 (x) | 2 (x) | 2 (x) | 2 (x) | 2 | 4 | 356 |
| KiCad-Library_Teensy_test_layout | 18 | 3 | 3 (x) | 2 | 2 (x) | 2 | 4 | 436 |
| rvi_v2x_hardware_V2X | 21 | 10 | 10 (x) | 4 | 4 (x) | 4 | 4 | 800 |
| 96boards-robomezzi_96boards-robomezzi | 2 | 1 | 1 (x) | 1 (x) | 1 (x) | 1 | 4 | 256 |
| TCKB_Kicad_TCKB | 22 | 8 | 4 | 3 | 3 (x) | 3 | 4 | 881 |
| LeeChee_1800 | 5 | 1 | 1 (x) | 1 (x) | 1 (x) | 1 | 2 | 627 |
| dorkyboard_keyboard | 3 | 1 | 1 (x) | 1 (x) | 1 (x) | 1 | 2 | 596 |
| mini_mass_prog_bench_prog_rig | 9 | 5 | 5 (x) | stopped | – | (5) | – | 1449 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 25 | 17 | 16 | 0 | – | 0 | 4 | 1461 |
| CoreOne-xCORE200-Original_CoreOne | 19 | 12 | 12 (x) | 10 | 10 (x) | 10 | 6 | 1657 |
| mechkeys_LFK78 | 45 | 31 | 27 | 12 | 8 | 8 | 6 | 1929 |
| Aleste-520EX_aleste | 250 | 182 | 159 | 1 | 0 | 0 | 6 | 2415 |
| poncho_fpga_fpga-dongle | 33 | 26 | 26 (x) | 7 | 6 | 6 | 6 | 1679 |
| MonApollo_analog-board | 44 | 30 | 22 | 14 | 12 | 12 | 6 | 2313 |
| zx-sizif-512-ext_sizif512ext | 54 | 38 | 26 | 12 | 10 | 10 | 6 | 1662 |
| kitspace_d20_tri_r0.3 | 142 | 128 | 127 | 127 (x) | 127 (x) | 127 | 4 | 2141 |
| **29 boards** | 881 | | | | | 219 | | 23,932 |

Eight of the 29 complete (Aleste, memsarray, sd_wav, fifogfx, ErgoDone, test-jig, MicroMaple, P8000); KiCad 10.0.6: 0 unconnected on all
eight (`finish/kicad-judge.json`). 881 open become 219, of which 127 are d20; the other 28 boards go from 739 to 92. Six more end 1 short.
Aleste-520EX: 250 → 182 → 159 → 1 → 0 in 40 minutes, at six layers.

mini_mass was stopped by hand in its fourth rung: at six copper layers the process passed 100 GB of memory and was still growing (20 M work,
8 variants; its searches there fail after about 21 M expansions each). The same board at `toplay4` is what exhausted the machine's memory at
15:05. `route --finish` now writes its output after every rung it keeps, so a ladder stopped part-way leaves its best board (it did not yet
when this was run: mini_mass's 5-open board was lost).

### The stronger ladder on the boards one or two short (`finish-strong/results.jsonl`)

`route --work 60000000 --finish --finish-fresh --repair-variants 8 --finish-masks --finish-layers 2,4`, eight boards, three at a time.

| Board | as given (60 M) | fitted rules | from the board as given | + pad masks | + 2 layers | + 4 layers | final | KiCad unconnected |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| Hardware_Playground_serial_gw | 0 | – | – | – | – | – | 0 | 0 |
| kitspace_teensy-fx | 0 | – | – | – | – | – | 0 | 0 |
| LeeChee_1800 | 0 | – | – | – | – | – | 0 | 0 |
| PCIE-to-MXM-Adapter | 30 | 0 | – | – | – | – | 0 | 0 |
| KiCad-Library_Teensy_test_layout | 16 | 1 | 0 | – | – | – | 0 | 0 |
| DronPi_dronPi | 11 | 8 | 9 (x) | 0 | – | – | 0 | 0 |
| 96boards-robomezzi | 2 | 1 | 1 (x) | 1 (x) | 1 (x) | 1 (x) | 1 | |
| dorkyboard_keyboard | 2 | 1 | 2 (x) | 0 | – | – | 0 | 0 |

Seven of the eight complete, all at the board's own layer count; DronPi, Teensy_test_layout and dorkyboard for the first time on any arm.
With them **19 of the 33 boards have a complete output** (3 before). Teensy_test_layout's output has 499 clearance violations more than the unrouted
board in KiCad: its fitted class clearance is what let it complete.

### The stronger ladder on the eleven boards still short (`finish-strong2/results.jsonl`, 2026-11-20)

The same command on the boards the 20 M ladder left 3 to 16 short (d20 and mini_mass left out), three at a time, each job with an address-space
limit of 45 GB (`scripts/finishrun4.py`). No job reached the limit. "+2 layers" and "+4 layers" are over the board's own count; (x) is a rung
that did not end with fewer open and was dropped.

| Board | own layers | as given (60 M) | fitted rules | from the board as given | + pad masks | + 2 layers | + 4 layers | final | KiCad unconnected | seconds |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| TCKB_Kicad_TCKB | 2 | 21 | 5 | 1 | 0 | – | – | 0 | 0 | 1816 |
| rvi_v2x_hardware_V2X | 2 | 9 | 3 | 3 (x) | 3 (x) | 0 | – | 0 | 0 | 1611 |
| CoreOne-xCORE200-Original_CoreOne | 4 | 2 | 0 | – | – | – | – | 0 | 0 | 655 |
| mechkeys_LFK78 | 2 | 32 | 18 | 19 (x) | 15 | 3 | 0 | 0 | 2 | 5892 |
| MonApollo_analog-board | 2 | 39 | 15 | 14 | 12 | 1 | 1 (x) | 1 |  | 4956 |
| kitspace_OtterPill | 2 | 8 | 5 | 6 (x) | 5 (x) | 2 | 2 (x) | 2 |  | 1222 |
| motor-high-current-sensors_CurrentSensorPCB_line | 2 | 5 | 3 | 5 (x) | 3 (x) | 3 (x) | 3 (x) | 3 |  | 877 |
| zx-sizif-512-ext_sizif512ext | 2 | 44 | 26 | 21 | 6 | 3 | 3 (x) | 3 |  | 3311 |
| Hardware_Playground_E3_Pcb_rev0.3 | 2 | 19 | 16 | 18 (x) | 16 (x) | 4 | 4 (x) | 4 |  | 1410 |
| poncho_fpga_fpga-dongle | 2 | 32 | 14 | 11 | 9 | 5 | 5 (x) | 5 |  | 3138 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 2 | 26 | 11 | 8 | 7 | 6 | 6 (x) | 6 |  | 1900 |

Four of the eleven end with 0 open by the router's count: CoreOne and TCKB at their own layer count, V2X with two more layers, LFK78 with
four more. KiCad 10.0.6 finds 0 unconnected on CoreOne, TCKB and V2X, and **2 unconnected on LFK78** (not counted as complete; not looked
into yet). With the three, **22 of the 33 boards have a complete output**. TCKB's output has 499 clearance and 199 hole-clearance violations
more than the unrouted board in KiCad (its fitted rules are what let it complete); V2X adds 9 copper-to-edge, CoreOne adds none. The other
seven end 1 to 6 short (MonApollo 1, OtterPill 2, motor 3, sizif 3, E3 4, poncho 5, EtherCAT 6), against 3 to 16 after the 20 M ladder.

### The from-nothing rung (`--finish --finish-fresh --finish-masks`, 20 M)

| Board | as given | fitted rules, on top | fitted rules, from the board as given | + pad masks | final |
|---|--:|--:|--:|--:|--:|
| PCIE-to-MXM-Adapter | 30 | 2 | 0 | – | 0 |
| KiCad-Library_Teensy_test_layout | 18 | 3 | 1 | 1 (x) | 1 |
| kitspace_OtterPill | 11 | 9 | 11 (x) | 9 (x) | 9 |

## Other arms

| Board | topfit | toprv8 | toplay2 | topfocus | fit3 | fitseed2 | fitseed3 | fitseed4 | fitseed5 |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 96boards-robomezzi_96boards-robomezzi | 1 | – | 1 | – | 2 | 2 | 2 | 2 | 2 |
| Aleste-520EX_aleste | 195 | – | 8 | – | 250 | – | – | – | – |
| CoreOne-xCORE200-Original_CoreOne | 14 | – | 14 | – | 19 | – | – | – | – |
| DronPi_dronPi | 9 | – | 9 | – | 9 | – | – | – | – |
| ErgodoneWithHole_ErgoDone | 0 | – | – | – | 0 | – | – | – | – |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 20 | – | 20 | – | 24 | – | – | – | – |
| Hardware_Playground_E3_Pcb_rev0.3 | 18 | – | 4 | – | 19 | – | – | – | – |
| Hardware_Playground_multisensor_cr2032 | 0 | – | – | – | 0 | – | – | – | – |
| Hardware_Playground_sd_wav_pcm5100 | 0 | – | – | – | 0 | – | – | – | – |
| Hardware_Playground_serial_gw_ATMEGA328P | 1 | – | 1 | – | 1 | 1 | 1 | 1 | 1 |
| KiCad-Library_Teensy_test_layout | 3 | – | 2 | – | 2 | – | – | – | – |
| LeeChee_1800 | 1 | – | 1 | – | 6 | – | – | – | – |
| MicroMaple_MicroMaple | 8 | – | 2 | – | 9 | – | – | – | – |
| MonApollo_analog-board | 35 | – | 18 | – | 45 | – | – | – | – |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 17 | – | 0 | – | 25 | – | – | – | – |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 2 | – | 1 | – | 0 | – | – | – | – |
| TCKB_Kicad_TCKB | 8 | – | 2 | – | 12 | – | – | – | – |
| dorkyboard_keyboard | 1 | – | 1 | – | 3 | – | – | – | – |
| fifogfx_c64cart | 0 | – | – | – | 0 | – | – | – | – |
| front-end-modules_LimeSDR_Sony | 0 | – | – | – | 1 | – | – | – | – |
| kitspace_OtterPill | 9 | – | 4 | – | 11 | – | – | – | – |
| kitspace_ardfpga | 6 | – | 3 | – | 7 | – | – | – | – |
| kitspace_d20_tri_r0.3 | 129 | – | 129 | – | 142 | – | – | – | – |
| kitspace_teensy-fx | 2 | – | 2 | – | 2 | – | – | – | – |
| kitspace_test-jig | 0 | – | – | – | 0 | – | – | – | – |
| mechkeys_58r-mxalps | 0 | – | – | – | 5 | – | – | – | – |
| mechkeys_LFK78 | 31 | – | 14 | – | 45 | – | – | – | – |
| memsarray_mems_modules | 1 | – | 1 | – | 1 | – | – | – | – |
| mini_mass_prog_bench_prog_rig | 7 | – | 7 | – | 9 | – | – | – | – |
| motor-high-current-sensors_CurrentSensorPCB_line | 3 | – | 3 | – | 5 | – | – | – | – |
| poncho_fpga_fpga-dongle | 27 | – | 17 | – | 45 | – | – | – | – |
| rvi_v2x_hardware_V2X | 11 | – | 2 | – | 18 | – | – | – | – |
| zx-sizif-512-ext_sizif512ext | 38 | – | 30 | – | 54 | – | – | – | – |
| **open** | 597 | 0 | 296 | 0 | 771 | 3 | 3 | 3 | 3 |
| **complete** | 7 | 0 | 1 | 0 | 6 | 0 | 0 | 0 | 0 |
| boards run | 33 | 0 | 26 | 0 | 33 | 2 | 2 | 2 | 2 |

Seed spread (fitseedN against fit3): robomezzi 2, 2, 2, 2, 2; serial_gw 1, 1, 1, 1, 1. On these two the seed changes nothing.

Focused repair (topfocus against toplay2, same layers): CoreOne 14 → 11 (3 of 47 attempts kept; unfocused 0 of 56), EtherCAT 20 → 15 (5 of 75;
unfocused 0 of 95). d20 not finished. On d20 at toplay2 every one of 126 unfocused attempts ended with more open than before (129 → 131..139).

## Best arm per board

| Board | base | best, no designer layout | arm | best, any | arm |
|---|--:|--:|---|--:|---|
| 96boards-robomezzi_96boards-robomezzi | 2 | 1 | c60fit | 1 | c60fit |
| Aleste-520EX_aleste | 250 | 0 | toplay4 | 0 | toplay4 |
| CoreOne-xCORE200-Original_CoreOne | 19 | 2 | ctl60 | 2 | ctl60 |
| DronPi_dronPi | 11 | 2 | topmask | 2 | topmask |
| ErgodoneWithHole_ErgoDone | 2 | 0 | fit3 | 0 | fit3 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 26 | 18 | topmask | 18 | topmask |
| Hardware_Playground_E3_Pcb_rev0.3 | 19 | 4 | toplay2 | 4 | toplay2 |
| Hardware_Playground_multisensor_cr2032 | 0 | 0 | base | 0 | base |
| Hardware_Playground_sd_wav_pcm5100 | 2 | 0 | fit3 | 0 | fit3 |
| Hardware_Playground_serial_gw_ATMEGA328P | 1 | 0 | ctl60 | 0 | ctl60 |
| KiCad-Library_Teensy_test_layout | 18 | 2 | fit3 | 2 | fit3 |
| LeeChee_1800 | 5 | 0 | ctl60 | 0 | ctl60 |
| MicroMaple_MicroMaple | 9 | 0 | c60lay2 | 0 | c60lay2 |
| MonApollo_analog-board | 44 | 16 | toplay4 | 16 | toplay4 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 25 | 0 | toplay2 | 0 | toplay2 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 30 | 0 | fit3 | 0 | fit3 |
| TCKB_Kicad_TCKB | 22 | 2 | toplay2 | 2 | toplay2 |
| dorkyboard_keyboard | 3 | 1 | topfit | 1 | topfit |
| fifogfx_c64cart | 40 | 0 | c60fit | 0 | c60fit |
| front-end-modules_LimeSDR_Sony | 0 | 0 | base | 0 | base |
| kitspace_OtterPill | 11 | 2 | c60lay2 | 0 | fitref3 |
| kitspace_ardfpga | 7 | 0 | c60fit | 0 | c60fit |
| kitspace_d20_tri_r0.3 | 142 | 129 | topfit | 119 | fitref3 |
| kitspace_teensy-fx | 2 | 0 | ctl60 | 0 | ctl60 |
| kitspace_test-jig | 32 | 0 | c60fit | 0 | c60fit |
| mechkeys_58r-mxalps | 0 | 0 | base | 0 | base |
| mechkeys_LFK78 | 45 | 12 | toplay4 | 12 | toplay4 |
| memsarray_mems_modules | 1 | 0 | c60mask | 0 | c60mask |
| mini_mass_prog_bench_prog_rig | 9 | 7 | topfit | 7 | topfit |
| motor-high-current-sensors_CurrentSensorPCB_line | 5 | 3 | topfit | 3 | topfit |
| poncho_fpga_fpga-dongle | 33 | 16 | toplay4 | 16 | toplay4 |
| rvi_v2x_hardware_V2X | 21 | 2 | toplay2 | 2 | toplay2 |
| zx-sizif-512-ext_sizif512ext | 54 | 26 | topmask | 26 | topmask |
| **open** | 890 | 245 | | 233 | |
| **complete** | 3 | 16 | | 17 | |

## KiCad on the designer layouts: tracks in pad mask openings

| Board | `solder_mask_bridge`, designer's layout | unrouted board | pad openings with another net's track (TraceMaker's count) |
|---|--:|--:|--:|
| memsarray_mems_modules | 7 | 0 | 5 |
| poncho_fpga_fpga-dongle | 118 | 0 | 159 |
| dorkyboard_keyboard | 69 | 0 | 51 |
| mechkeys_LFK78 | 56 | 0 | 44 |
| zx-sizif-512-ext_sizif512ext | 3 | 0 | 3 |
| ErgodoneWithHole_ErgoDone | 8 | 0 | 12 |
| mechkeys_58r-mxalps | 23 | 0 | 19 |

fifogfx (40) is the card-edge case of D194. DronPi and PCIE have 1 each (under the evidence threshold of 3).

## What is left

- d20 (129): layers do nothing on top; 27 pins are walled in by the earlier rungs' own copper. Not measured with focused repair.
- Boards one or two short (robomezzi, dorkyboard, PCIE on top, TCKB, V2X, CoreOne at 60 M): each open pin is walled in by a neighbour's
  track that the repair lifts and the router lays back the same way.
- Memory: mini_mass at six layers passes 100 GB (above). One variant on one thread under a 30 GB cap ends in `std::bad_alloc` after 34 s,
  before any routing: the size of the search state, not a leak. The router does not check memory before it starts.
- On top is not always the better order: PCIE completes fitted from nothing and is left 1-2 short on top; OtterPill completes only fitted from
  the designer's layout from nothing. `route --finish --finish-fresh` adds the from-nothing order as a rung (above).
- No change to the default route: every option here is off unless asked for. Quick tier: 30 of 30 outputs byte-identical (`finq` against `fitq-new`).

## Files

- `repair/results.jsonl`: one row per board and repair arm. `ontop/results.jsonl`: one row per board and arm of the ladders.
- `finish-strong/`: the stronger ladder on eight boards, and KiCad on its completes.
- `finish/results.jsonl`, `finish/kicad-judge.json`: `route --finish` end to end, and KiCad on its seven complete outputs.
- `ontop/kicad-judge.json`: KiCad 10.0.6 on the newly complete outputs (unconnected items; violations added over the unrouted board).
- `scripts/`: the run scripts (`ROOT` = the repository; the binaries were frozen copies of the build at each step).

## Complete for KiCad too, and rip-up chains applied (2026-11-20)

**LFK78** run again with the stronger ladder and the clean-up of copper that joins no pad (D198): 32 → 18 → 15 → 3 (four layers) → 0 (six
layers), 5,601 s; one piece taken off on each layer rung (3 and 2 tracks and vias of the rung before). KiCad 10.0.6: 0 unconnected, no
repeated id (`finish-strong3/`). Before the fix the same ladder ended 0 for the router and 2 for KiCad.

**Half the lattice pitch** on top of the best output of four boards (20 M, fitted rules, focused repair): MonApollo 1, robomezzi 1,
OtterPill 2, motor 3 open, as before. **Rip-up chains applied** (`tracemaker chains --apply`, D200) on the same outputs, KiCad's count
(`chains-2026-11-20/`):

| board | after the ladder | chains (depth 4) | again, depth 8 |
|---|--:|--:|--:|
| MonApollo_analog-board | 1 | **0** | |
| kitspace_OtterPill | 2 | 1 | |
| zx-sizif-512-ext | 3 | 2 | 2 |
| Hardware_Playground_E3 | 4 | 4 (all `no_path_bare`) | |
| poncho_fpga | 5 | 3 | **1** |
| EtherCAT_shield_v1 | 6 | 5 | 5 (router 4) |

`route --finish --finish-chains` on MonApollo's output: as given 1, fitted rules 1, rip-up chains 0; KiCad 0 unconnected
(`MonApollo-finish-chains.*`). **24 of 33 boards now have a complete output** (LFK78 and MonApollo added). d20 (127 open), measured
only: 22 close one at a time (5 joint, 17 depth 1-4), 93 too deep at depth 4, 12 lift a net with no path (`d20-chains.json`); applied one
after the other (`chains --apply`, depth 4): 15 close, 127 → 111 open, KiCad 111 unconnected (`d20-apply.json`, `d20-chained.drc.json`).

Memory (D199): mini_mass at eight layers on top of its six-layer result, one variant, 20 GB cap: `std::bad_alloc` in 3 s before, 10.3 GB
peak and the run to the end after (16 searches with one direction state, 0 refused).
