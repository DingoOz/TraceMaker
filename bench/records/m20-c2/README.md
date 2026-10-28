# M20 C2: pin-swap feasibility with a free-assignment oracle (2026-10-27)

Design: doc 05 §56, decision D141, doc 18 §11. Binary: branch `m20-c5-fanout` (main ec7cc9b plus the C5 and C2 tools; routing
unchanged with the options off), release build, `--no-gpu`, 8 threads (4 on the quick boards), 3 boards at a time (2 on the quick
boards) on the shared server. KiCad 10.0.6 judges every board with `--refill-zones` (`bench/run.py --refill`).

**Question** (doc 18 C2): would the freedom to swap pins change routed count, vias or layers enough to build a pass? Swappable
groups are found by construction, not from the designs (`bench/pin_swap.py`, assumption A73): connector signal pins (reference J, P,
CN, CON or X; one pad type and size; not a supply, ground or differential-pair net; one pad of the net on the connector) and
resistor arrays (RN/RP with 4, 8 or 16 pads, resistors as units, either way round). The oracle assigns the nets to the pads by a
minimum-cost assignment of the distance from each pad to the nearest pad of its net elsewhere (scipy's `linear_sum_assignment`).
The swapped board permutes the pads' own `(net ...)` nodes; everything else is byte-identical (`groups-*.json` lists every group,
its nets and the assignment).

## Runs

| Run | What | Boards |
|---|---|---|
| `runs/c2-base`, `runs/c2-swap` | as designed, and with the oracle's nets; 10 M work | the 20 of the 33 failing tier B-D boards (`pcbench-bd-fail`) that have a group the oracle shortens |
| `runs/c2q-base`, `runs/c2q-swap` | the same at 1 M work | the 10 of the 30 `quick` boards that have one |

```
bench/pin_swap.py scan --set pcbench-bd-fail --json groups-bd.json
bench/pin_swap.py make --boards <the 20> --out build/scratch/pinswap/fix
bench/run.py --boards <the 20> --work 10000000 --refill --jobs 3 --threads 8 --view --name c2-base --route-args=--no-gpu
bench/run.py --fixtures build/scratch/pinswap/fix --boards <the 20> --work 10000000 --refill --jobs 3 --threads 8 --view --name c2-swap --route-args=--no-gpu
bench/pin_swap.py compare --base c2-base --swap c2-swap --name c2-pinswap --groups groups-bd.json
(the same with --set quick, --work 1000000, --jobs 2 --threads 4: c2q-base, c2q-swap)
```

## Result

### The 20 failing boards (`pinswap-bd.md`)

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

### The 10 quick boards (`pinswap-quick.md`)

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

## Reading

- The oracle shortens the swapped groups' ratsnest by 0 to 12 % on the failing boards (MicroMaple 611 → 535 mm) and 0 to 19 % on the quick ones (scimpy 60.5 → 48.9 mm). Routed, that is 3 % fewer vias and 1 %
  less copper on the failing boards and no change in what routes: 982 → 978 open connections, 5 boards better and 5 worse, by 1 to 5
  connections each, about what a different seed gives. No board becomes clean. On the quick boards: 27 → 26 open, 228 → 224 vias,
  7 → 6 clean (Inkjet loses one connection).
- The gate of doc 18 C2 ("build the pass only if a median gain is clear") is not met: the median change in open connections is 0 on
  both sets. The proposal-only pin-swap pass is not built. This is an upper bound: a real design can swap fewer pins than the
  oracle, which assumes every connector signal pin is interchangeable.
- Layers were not measured: the layer advisor (D138) was not run on the swapped copies. With no change in open connections at the
  boards' own counts, a board that needs fewer layers after the swaps is unlikely, but not excluded.
