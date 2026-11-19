# Why hard boards stay open: the designer oracle, the rule fit and the chain measurement (2026-11-19)

Design: doc 20 §1.1, §2.8, §2.9; decision D194. Branch `rule-fit` (from `m26-impl` a316aae). Boards: the 33 failing PCBench boards of
tiers B-D (`bench/sets/pcbench-bd-fail.txt`) and three srj20 boards. The question was the user's: these boards never reach 100 %; rules
may be relaxed and compute spent; what is the cause and what can be done?

## Summary

1. **Two different causes.** Inside the designer's own finished layout the router puts back the nets it leaves open on some boards
   (a search problem: its own other routes are in the way) and cannot put back even a random quarter of the designer's nets on others
   (a modelling problem: the board, as designed, breaks rules the router is held to).
2. **The modelling problem is mostly solder-mask openings and a few minimums.** 141 of the 261 open connections of the best earlier
   results cannot be routed with every other route lifted. On fifogfx, test-jig, PCIE and ErgoDone the cause is a solder-mask opening
   that already exposes another net's pad on the board itself (card-edge fingers on both sides; solder jumpers).
3. **`route --fit-rules` relaxes exactly what the board's own copper already breaks.** From nothing, at equal budget, the 33 boards go
   from 890 open and 3 complete to 771 open and 6 complete (fitted to the board's own pads) or 734 open and 8 complete (fitted to the
   designer's layout). Six boards that no earlier run had completed now complete: fifogfx, test-jig, PCIE, ErgoDone, sd_wav, OtterPill
   (KiCad: 0 unconnected, no short; violations of the stated rules where the fit relaxed them). Three boards get worse.
4. **The search problem is shallow at the first step and deep after it.** Of the 120 open connections that have blockers, 62 are
   blocked by one net and 27 by two. Yet only 41 close by a rip-up chain of up to four levels (17 of them by routing the connection and
   its blockers together). On three srj20 ball-grid boards no chain closes.

## 1. Ledger (`scripts/ledger.py`)

Over every `boards.jsonl` of every worktree (about 4,000 board runs of these 33 boards): 11 boards were complete in KiCad's judgement
at least once, 22 never. The best result of each board, restricted to runs on its own layer count, is in `compare.json` (`path`,
`open`). Those boards are the starting point of §2 and §5.

## 2. Designer oracle (`scripts/oracle.py`, `oracle/results.jsonl`)

`processed.kicad_pcb` is the designer's routing without the zones. Some nets' tracks and vias are removed and the router routes only
those nets (`--only-net`), everything else fixed: 20 M work, 8 variants, `--no-kb --no-gpu --seed 1`. Three arms: the nets our best
run left open; a random 25 % of the nets that have tracks; a random 50 % (zone nets left out of all three).

| Board | Open in our best run | Those nets put back: open / connections | Random 25 %: open / connections | Random 50 %: open / connections |
|---|--:|--:|--:|--:|
| kitspace_OtterPill | 4 | 3 / 5 | 2 / 16 | 7 / 56 |
| memsarray_mems_modules | 1 | 1 / 3 | 0 / 1 | 0 / 1 |
| MicroMaple_MicroMaple | 3 | 2 / 7 | 0 / 16 | 1 / 42 |
| 96boards-robomezzi_96boards-robomezzi | 1 | 0 / 1 | 0 / 42 | 2 / 85 |
| poncho_fpga_fpga-dongle | 9 | 0 / 10 | 3 / 55 | 11 / 106 |
| ErgodoneWithHole_ErgoDone | 2 | 2 / 2 | 0 / 68 | 2 / 170 |
| Hardware_Playground_sd_wav_pcm5100 | 1 | 0 / 1 | 0 / 14 | 1 / 24 |
| Hardware_Playground_E3_Pcb_rev0.3 | 15 | 15 / 35 | 13 / 72 | 14 / 154 |
| kitspace_test-jig | 30 | 25 / 59 | 3 / 40 | 15 / 113 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 10 | 0 / 22 | 4 / 29 | 11 / 65 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 16 | 7 / 26 | 5 / 37 | 2 / 76 |
| rvi_v2x_hardware_V2X | 8 | 0 / 41 | 1 / 57 | 1 / 126 |
| DronPi_dronPi | 11 | 8 / 23 | 3 / 72 | 1 / 118 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 28 | 21 / 36 | 8 / 21 | 11 / 34 |
| MonApollo_analog-board | 28 | 1 / 149 | 2 / 275 | 7 / 490 |
| dorkyboard_keyboard | 2 | 0 / 25 | 0 / 100 | 0 / 269 |
| zx-sizif-512-ext_sizif512ext | 30 | 17 / 108 | 13 / 116 | 25 / 235 |
| TCKB_Kicad_TCKB | 15 | 3 / 45 | 2 / 155 | 7 / 190 |
| fifogfx_c64cart | 40 | 40 / 77 | 8 / 40 | 19 / 60 |
| mini_mass_prog_bench_prog_rig | 2 | 0 / 12 | 0 / 111 | 1 / 197 |
| kitspace_d20_tri_r0.3 | 123 | 153 / 488 | 60 / 117 | 109 / 290 |
| mechkeys_LFK78 | 17 | 2 / 115 | 1 / 151 | 0 / 274 |
| Aleste-520EX_aleste | 162 | 104 / 433 | 28 / 730 | 41 / 719 |
| motor-high-current-sensors_CurrentSensorPCB_line | 1 | — | 0 / 62 | 2 / 139 |

- **Search boards** (the router's open nets route once the rest is the designer's): robomezzi, poncho, sd_wav, P8000, V2X,
  dorkyboard, mini_mass (0 left); MonApollo (1 of 149), LFK78 (2 of 115), TCKB (3 of 45).
- **Modelling boards** (the designer's own nets do not go back): d20, fifogfx, Aleste, PCIE, sizif, E3, test-jig, EtherCAT, OtterPill,
  DronPi.
- The random arms can pick supply nets the designer partly poured (their tracks stop at a zone that is no longer there); that
  overstates the open count of the random arms on boards with many pours.

### What the designers' layouts break (`drc/designer-layouts.txt`, the engine's DRC on `processed.kicad_pcb`)

All 33 boards are in the KiCad 5 file format, which has no copper-to-edge or hole clearance. KiCad 10 (and so the router and the
judge) applies its defaults, 0.5 mm and 0.25 mm, and ignores project rules on a board that carries legacy settings
(`io/kicad/project_reader.cpp`). The layouts break them: d20 579 hole-clearance and 81 edge errors, Aleste 173 edge errors (card-edge
pads), OtterPill 290 track-width and 235 clearance errors, E3 168 clearance errors.

### One rule at a time on sizif512ext (`oracle/sizif-one-rule.jsonl`)

A first test waived several minimums at once (`oracle/edge-waived.jsonl`: sizif 13 → 1 open, d20 60 → 40, Aleste 28 → 20, PCIE 8 → 8;
on fifogfx and Aleste the board kept its legacy settings, so the waiver did not fully apply). It was read as "the edge clearance".
Waiving one minimum at a time on sizif's random-25 % board corrects that:

| Waived | Open of 116 |
|---|--:|
| nothing | 13 |
| copper-to-edge clearance 0.5 → 0 | 14 |
| hole clearance 0.25 → 0 | 13 |
| minimum track width 0.2 → 0.1 | **2** |
| minimum via 0.45 / 0.3 → 0.4 / 0.2 | 13 |

The open connections are supply pins of U11, a QFN-48 at 0.4 mm pitch, in classes of 0.5 and 1.0 mm tracks. The designer leaves them
with 0.2 mm tracks: at 0.4 mm pitch and 0.2 mm clearance that fits with no slack, only on the pad's centre line. The lattice (0.065 mm)
does not hold that line, so the neck-down at the board's 0.2 mm minimum fails and a thinner one succeeds. This is a lattice loss at the
pin, not a rule the designer broke; the rule fit does not address it (§6).

## 3. Other differences from the designers (`scripts/compare.py`, `scripts/zshare.py`, `compare.json`)

- **Vias.** The router uses two to three times the designer's vias on the search boards: LFK78 895 against 340, MonApollo 782
  against 262, dorkyboard 179 against 68, sd_wav 83 against 24, 58r-mxalps 276 against 77.
- **Pour nets as tracks.** The fixtures have no zones. The router spends 10-46 % of its track length on nets the designer poured:
  dorkyboard GND 2,449 mm against the designer's 200 mm, LFK78 2,506 against 127, E3 761 against 51. Generated pours were measured in
  October (`bench/records/m20-c3`: 1,368 → 1,290 open at 10 M), so this is not the whole cause.
- **Class widths.** TCKB's default class asks 0.406 mm; 90 % of the designer's track length is 0.25 mm. OtterPill asks 0.25 mm and the
  designer uses 0.2 mm and 0.157 mm.

## 4. Rule fit (`route --fit-rules`, `--fit-rules-from BOARD`; `scripts/fitrun*.py`, `fit/results.jsonl`)

From nothing (`unrouted.kicad_pcb`), 20 M work, 8 variants on 8 threads, `--no-kb --no-gpu --seed 1 --time 1800`, six boards at a
time. Open connections; bold = complete.

| Board | Connections | Base | Fit, own pads | Fit, designer's layout | Mask openings made soft | Values relaxed (designer's layout, mm) |
|---|--:|--:|--:|--:|---|---|
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 272 | 26 | 24 | 24 | 8 pad openings | class clearance 0.2→0.177; via_annular_width 0.1→0.05 |
| Hardware_Playground_E3_Pcb_rev0.3 | 341 | 19 | 19 | 15 |  | hole_clearance 0.25→0; class clearance 0.2→0.09; class via_diameter 0.8→0.61; class via_drill 0.4→0.305 |
| Hardware_Playground_serial_gw_ATMEGA328P | 172 | 1 | 1 | 1 |  | copper_edge_clearance 0.5→0.245; via_annular_width 0.1→0.05 |
| MicroMaple_MicroMaple | 111 | 9 | 9 | 9 |  |  |
| kitspace_OtterPill | 137 | 11 | 11 | **0** |  | copper_edge_clearance 0.5→0.225; class clearance 0.2→0.157; track_width 0.2→0.1; class track_width 0.25→0.2; class via_diameter 0.8→0.6; class via_drill 0.4→0.3 |
| fifogfx_c64cart | 104 | 40 | **0** | **0** | 40 pad openings | copper_edge_clearance 0.5→0 |
| kitspace_ardfpga | 214 | 7 | 7 | 7 |  | class clearance 0.152→0.152 |
| kitspace_teensy-fx | 196 | 2 | 2 | 1 |  | class clearance 0.2→0.199 |
| memsarray_mems_modules | 10 | 1 | 1 | 1 |  | copper_edge_clearance 0.5→0 |
| kitspace_test-jig | 292 | 32 | **0** | **0** | 65 drawn openings | copper_edge_clearance 0.5→0.3; class clearance 0.2→0.141 |
| poncho_fpga_fpga-dongle | 413 | 33 | 45 | 45 | 110 pad openings | through_hole_diameter 0.406→0.4 |
| 96boards-robomezzi_96boards-robomezzi | 282 | 2 | 2 | 2 |  | class clearance 0.178→0.177 |
| DronPi_dronPi | 352 | 11 | 9 | 6 | 7 pad openings | class clearance 0.2→0.167; track_width 0.2→0.2; via_annular_width 0.1→0.05 |
| Hardware_Playground_multisensor_cr2032 | 205 | **0** | **0** | **0** | 4 pad openings | copper_edge_clearance 0.5→0.174; class clearance 0.25→0.175; via_annular_width 0.1→0.05 |
| ErgodoneWithHole_ErgoDone | 356 | 2 | **0** | **0** | 8 pad openings, 4 drawn openings | hole_clearance 0.25→0.0381; class clearance 0.305→0.09; via_annular_width 0.1→0.05 |
| Hardware_Playground_sd_wav_pcm5100 | 396 | 2 | **0** | **0** |  | copper_edge_clearance 0.5→0.264; class clearance 0.25→0.2; via_annular_width 0.1→0.05 |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 188 | 30 | **0** | **0** | 2 drawn openings | copper_edge_clearance 0.5→0.2; class track_width 0.25→0.2; class via_diameter 0.8→0.6; class via_drill 0.4→0.3 |
| kitspace_d20_tri_r0.3 | 553 | 142 | 142 | 119 |  | copper_edge_clearance 0.5→0.177; hole_clearance 0.25→0.232 |
| mechkeys_58r-mxalps | 361 | **0** | 5 | 5 | 8 pad openings | copper_edge_clearance 0.5→0.211; hole_clearance 0.25→0; class clearance 0.191→0.09 |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 361 | 25 | 25 | 25 |  |  |
| motor-high-current-sensors_CurrentSensorPCB_line | 472 | 5 | 5 | 5 |  | copper_edge_clearance 0.5→0.35; hole_clearance 0.25→0.2; class clearance 0.2→0.195; via_annular_width 0.1→0.05 |
| rvi_v2x_hardware_V2X | 479 | 21 | 18 | 17 |  | copper_edge_clearance 0.5→0; hole_clearance 0.25→0.207; via_annular_width 0.1→0.05 |
| zx-sizif-512-ext_sizif512ext | 498 | 54 | 54 | 54 |  | copper_edge_clearance 0.5→0.25; hole_clearance 0.25→0.2 |
| Aleste-520EX_aleste | 1616 | 250 | 250 | 250 |  | copper_edge_clearance 0.5→0 |
| KiCad-Library_Teensy_test_layout | 1268 | 18 | 2 | 2 | 63 pad openings | class clearance 0.254→0.178; via_annular_width 0.1→0.05 |
| CoreOne-xCORE200-Original_CoreOne | 807 | 19 | 19 | 14 |  | class via_diameter 0.8→0.6; class via_drill 0.5→0.3 |
| LeeChee_1800 | 1160 | 5 | 6 | 22 | 1 drawn openings | copper_edge_clearance 0.5→0; hole_clearance 0.25→0; class clearance 0.2→0.09 |
| MonApollo_analog-board | 1190 | 44 | 45 | 44 |  | class clearance 0.203→0.203; via_annular_width 0.1→0.0635 |
| TCKB_Kicad_TCKB | 597 | 22 | 12 | 9 |  | copper_edge_clearance 0.5→0; hole_clearance 0.25→0; class clearance 0.203→0.09; class track_width 0.406→0.25 |
| dorkyboard_keyboard | 830 | 3 | 3 | 3 |  | copper_edge_clearance 0.5→0.42; hole_clearance 0.25→0.16 |
| front-end-modules_LimeSDR_Sony | 814 | **0** | 1 | **0** | 2 drawn openings | copper_edge_clearance 0.5→0.152; hole_clearance 0.25→0.2; class clearance 0.1→0.09; via_annular_width 0.1→0.075 |
| mechkeys_LFK78 | 893 | 45 | 45 | 45 | 2 pad openings | hole_clearance 0.25→0 |
| mini_mass_prog_bench_prog_rig | 995 | 9 | 9 | 9 |  | hole_clearance 0.25→0.19; via_annular_width 0.1→0.05 |

| Arm | Open (33 boards) | Complete | Better / worse than base |
|---|--:|--:|---|
| base | 890 | 3 | |
| fit to own pads, minimums and clearance only (`fit`, before the mask openings) | 872 | 2 | 4 / 4 |
| the same without the clearance (`fit-noclr`) | 891 | 2 | 1 / 2 |
| fit to the designer's layout, before the mask openings (`fitref`) | 838 | 4 | 9 / 2 |
| the same without the clearance (`fitref-noclr`) | 852 | 1 | 6 / 4 |
| **fit to own pads, as built** (`fit3`) | **771** | **6** | 10 / 5 |
| **fit to the designer's layout, as built** (`fitref3`) | **734** | **8** | 15 / 3 |
| best of base and `fit3`, per board | 751 | 8 | |
| best of base and `fitref3`, per board | 700 | 9 | |

- **What closes boards is the mask openings.** fifogfx: 40 pad openings (card-edge fingers on both sides, each opening both masks, so
  every back finger lies in a front finger's opening and no track may leave any of them): 40 open → 0 in 57 s. test-jig: 65 openings
  drawn as graphics over pads (solder jumpers): 32 → 0. PCIE: 2 drawn openings: 30 → 0. ErgoDone: 8 + 4: 2 → 0.
- **The class values close OtterPill** (11 → 0): the designer's via 0.6 / 0.3 mm and track 0.2 mm instead of the class's 0.8 / 0.4 and
  0.25.
- **The clearance is worth having and is the volatile part**: 838 against 852 with it, but LeeChee goes 5 → 22 and 58r-mxalps 0 → 5.
  A lower clearance makes the lattice finer (pitch = (width + clearance) / 6, doc 05 §25), and a finer lattice spends the budget on
  more steps per path (doc 20 §2.7). The ladder should keep the better of the plain and the fitted run.
- **Worse boards:** poncho 33 → 45 (110 pad openings made soft), 58r-mxalps 0 → 5, LeeChee 5 → 22 with the designer's layout. Why
  poncho loses was not looked into.
- **No effect** where the fit relaxes only the edge or hole clearance (sizif, Aleste, LFK78, d20 partly): the copper is nearly the
  same as the base run's. Those boards' opens are elsewhere.

### KiCad's judgement (`fit/kicad-judge.json`, `scripts/judge.py`, kicad-cli 10.0.6)

| Board | Arm | Unconnected | Errors added against the stated rules |
|---|---|--:|---|
| fifogfx_c64cart | own pads | 0 | solder_mask_bridge 41, copper_edge_clearance 3 |
| kitspace_test-jig | own pads | 0 | clearance 167, solder_mask_bridge 38 |
| PCIE-to-MXM-Adapter | own pads | 0 | solder_mask_bridge 131, copper_edge_clearance 21 |
| ErgodoneWithHole_ErgoDone | own pads | 0 | annular_width 34 |
| sd_wav_pcm5100 | own pads | 0 | clearance 21 |
| kitspace_OtterPill | designer's layout | 0 | clearance 81, track_width 59, copper_edge_clearance 28 |
| multisensor_cr2032 | own pads | 0 | clearance 35, copper_edge_clearance 7 |
| LimeSDR_Sony | designer's layout | 0 | annular_width 199+, hole_clearance 199+, copper_edge_clearance 13 (KiCad caps a type at 199) |

No run added a short. The added errors are the price of the fit: the output breaks the stated rules where the board or the designer's
layout already did. The designers' own layouts carry the same kinds (fifogfx 44 solder_mask_bridge, PCIE 95, test-jig 29;
`bench/results/human-baseline.json`).

## 5. Chain measurement (`tracemaker chains`; `chains/*.json`)

On each board's best earlier result, every open connection alone, the rest of the routed board fixed: 10 M baseline, 2 M per run,
depth ≤ 4, ≤ 32 nets lifted, blockers lifted within 1 mm of the new route. Aleste (162 open) was not finished. d20 finished after the
table was made and is not in its totals: 120 open, none of them without a route on the bare board, 6 closed joint, 4 at depth 1,
1 at depth 3, 95 still open at depth 4 and 14 with a lifted net left without a way (`chains/kitspace_d20_tri_r0.3.json`).

| Board | Open | closed joint | closed depth 1 | closed depth 2 | closed depth 3 | closed depth 4 | too deep | lifted net no path bare | too many nets | blockers not found | no path bare |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| 96boards-robomezzi_96boards-robomezzi | 1 |  |  |  |  |  |  | 1 |  |  |  |
| DronPi_dronPi | 10 |  |  |  |  |  |  |  |  |  | 10 |
| ErgodoneWithHole_ErgoDone | 2 |  |  |  |  |  |  |  |  |  | 2 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 14 | 2 |  | 1 |  |  | 3 | 8 |  |  |  |
| Hardware_Playground_E3_Pcb_rev0.3 | 15 |  |  |  |  |  |  |  |  |  | 15 |
| Hardware_Playground_sd_wav_pcm5100 | 1 |  | 1 |  |  |  |  |  |  |  |  |
| MicroMaple_MicroMaple | 3 | 1 |  |  |  |  | 2 |  |  |  |  |
| MonApollo_analog-board | 27 | 7 | 4 | 2 | 1 | 1 | 10 | 2 |  |  |  |
| P8000_WDC_Emulator_P8000_WDC_Emulator | 10 | 2 | 2 |  |  | 1 | 5 |  |  |  |  |
| PCIE-to-MXM-Adapter_PCIEx1toMXM3.0 | 28 |  |  |  |  |  | 2 |  |  |  | 26 |
| TCKB_Kicad_TCKB | 12 |  | 1 | 1 | 2 | 1 | 5 | 2 |  |  |  |
| dorkyboard_keyboard | 1 |  |  |  |  |  | 1 |  |  |  |  |
| fifogfx_c64cart | 40 |  |  |  |  |  |  |  |  |  | 40 |
| kitspace_OtterPill | 4 |  |  |  |  |  | 2 | 1 |  |  | 1 |
| kitspace_test-jig | 31 | 1 |  |  |  |  |  |  |  |  | 30 |
| mechkeys_LFK78 | 15 | 1 | 1 | 1 |  | 1 | 9 |  | 2 |  |  |
| memsarray_mems_modules | 1 |  |  |  |  |  |  |  |  |  | 1 |
| mini_mass_prog_bench_prog_rig | 1 |  |  |  |  |  |  | 1 |  |  |  |
| poncho_fpga_fpga-dongle | 9 |  |  | 2 |  |  | 7 |  |  |  |  |
| rvi_v2x_hardware_V2X | 6 | 3 |  |  |  |  | 2 | 1 |  |  |  |
| zx-sizif-512-ext_sizif512ext | 30 |  |  |  | 1 |  | 13 |  |  |  | 16 |
| **Total** | 261 | 17 | 9 | 7 | 4 | 4 | 61 | 16 | 2 |  | 141 |

- **no path bare (141):** the connection does not route with every other route lifted. These are the modelling opens of §2 and §4:
  fifogfx 40, test-jig 30, PCIE 26, sizif 16, E3 15, DronPi 10.
- **First blockers** (120 connections): 1 net for 62, 2 for 27, 3 for 11, 4 for 6, more for 14. The median open connection is kept
  out by one net.
- **Closed: 41 of 120** (34 %). 17 by routing the connection and its blockers together ("joint"), 9 with every blocker routing again
  at once, 15 at depth 2 to 4.
- **Not closed: 79.** 61 chains are still open four levels down. In 16 a lifted net has no way left at all once the connection is in:
  the connection's route, chosen with the blocker absent, took the blocker's only way.
- **Held-first order** (`chains/held-first/`, 15 boards, before the joint step): 10 closed of 43 with blockers. The joint step is what
  moves the measure; an order that fixes the new connection first loses.
- **A first version** (not kept) let a placed connection be lifted again by the next level: two nets swapped places until the depth
  limit, and "too deep" was 91.

### srj20 (`chains/srj20/`, 5 M per run)

sample152, 040 and 070, two open each, on their best boards of the ramp campaign: first blockers 2 to 6; neither the joint step nor a
chain closed any of the six within four levels (9 to 14 nets lifted each). The ball gets out when a few nets are lifted, and those
nets cannot be put back without lifting others, level after level. That agrees with the M26 record (the congestion is board-wide)
and points away from chains and towards an exact model of the interstice grid for these boards (doc 20 §2.10).

## 6. What this leaves

- **Pin escapes with no slack** (sizif U11; probably part of EtherCAT's QFN and E3's U3): the designer's track fits only on the pad's
  centre line, and the lattice misses it. An escape stub laid off the lattice (`--escape-stubs` exists, opt-in, D126) is the direction.
- **PCIE without the mask fit, d20, Aleste, E3:** d20 improves with the designer's edge and hole values (142 → 119) and stays far
  from complete; E3's net-less copper graphics need `--soft-copper-graphics` (doc 20 §2.4); Aleste is unchanged by the fit.
- **The search boards:** a joint re-route of an open connection with its minimal blockers closes about a third of the opens that have
  blockers. That is a candidate last step of the default route, to be measured against equal extra budget (doc 18 M22 rule).
- **Ball grids:** an exact model (doc 20 §2.10).

## Files

| Path | What |
|---|---|
| `scripts/` | `ledger.py`, `compare.py`, `zshare.py`, `oracle.py`, `fitrun.py` (base, fit, fitref), `fitrun2.py` (no clearance), `fitrun3.py` (as built), `judge.py`. `ROOT` and `SCRATCH` stand for the worktree and a scratch folder |
| `compare.json` | per board: best earlier result, open nets, designer's and our vias and lengths, zone nets |
| `oracle/` | `results.jsonl` (three arms), `edge-waived.jsonl`, `sizif-one-rule.jsonl` |
| `drc/designer-layouts.txt` | the engine's DRC counts on 14 designers' layouts |
| `fit/` | `results.jsonl` (seven arms × 33 boards), `kicad-judge.json` |
| `chains/` | one JSON per board; `held-first/`; `srj20/` |

Routed boards are not kept (PCBench and srj20 boards are not ours to redistribute).

## No change to the default route

With the options off, the build with the rule fit and the base build (the same source before it) give byte-identical boards on the
quick tier: `bench/run.py --set quick --work 1000000 --route-args "--variants 1"`, 30 of 30 identical (runs `fitq-base`, `fitq-new`;
both 53.3 % clean pass, 93.9 % completion at that budget). `ctest --preset release`: no failure, 2 skipped as before (`delay_room`,
`fixture_preparation_native`). New tests: `tests/test_fit_rules.cpp` (4 cases: edge fit and its evidence threshold, fit from an
earlier layout, the bridged mask opening, the chain measurement). The asan preset was not run.
