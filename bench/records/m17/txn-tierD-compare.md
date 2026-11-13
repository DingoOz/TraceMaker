# txn-base-tierD → txn-txn-tierD

22 boards; routed boards byte-identical: 4/22.
Clean pass 9 → 8; mean completion 97.0% → 97.5%; router seconds 2454.5 → 3262.7.

| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |
|---|---|---|---|---|---|---|---|
| Aleste-520EX_aleste | 1426 → 1434 / 1616 | 61.9% → 63.5% | no → no | – | 404.1 → 377.4 | 2775.3 → 2656.9 | differs |
| DSP-ADAU1452_DSP-ADAU1452 | 742 → 742 / 742 | 100.0% → 100.0% | yes → yes | – | 74.7 → 112.9 | 417.8 → 554.5 | differs |
| Horticulture_Light_EQ_3-Band_light | 422 → 422 / 422 | 100.0% → 100.0% | yes → yes | – | 36.4 → 75.0 | 215.7 → 286.9 | differs |
| Keyboard_PCB_Keyboard | 534 → 534 / 534 | 100.0% → 100.0% | yes → yes | – | 124.1 → 131.5 | 913.3 → 1003.6 | differs |
| LeeChee_1800 | 1160 → 1159 / 1160 | 100.0% → 99.8% | no → no | – | 275.6 → 419.4 | – | differs |
| TCKB_Kicad_TCKB | 576 → 575 / 597 | 95.8% → 95.6% | no → no | – | 401.0 → 384.1 | 2604.6 → 2460.5 | differs |
| m2-electronics_m2fc | 724 → 723 / 724 | 100.0% → 99.8% | yes → no | – | 120.3 → 412.0 | 479.9 → 2724.9 | differs |
| memsarray_mems_array | 245 → 245 / 245 | 100.0% → 100.0% | yes → yes | – | 121.8 → 121.5 | 933.8 → 808.7 | differs |
| newer-motor-controllers_busparts | 316 → 316 / 316 | 100.0% → 100.0% | yes → yes | – | 122.7 → 123.2 | 790.2 → 772.9 | differs |
| newer-motor-controllers_si31-3 | 316 → 316 / 316 | 100.0% → 100.0% | yes → yes | – | 123.1 → 122.5 | 783.2 → 756.5 | differs |
| opendous_Upconverter | 813 → 813 / 813 | 100.0% → 100.0% | yes → yes | – | 33.3 → 25.1 | 147.5 → 107.6 | differs |

Style (informational): bends per connection 0.8032 → 0.7494; acute angles 1 → 5; stair steps 49 → 43; short segments 2365 → 2620; off angle segments 2795 → 5101; detour ratio 1.1144 → 1.1014; long detours 205 → 241.

| Board | Bends per connection | Acute angles | Stair steps | Short segments | Off angle segments | Detour ratio | Long detours |
|---|---|---|---|---|---|---|---|
| Aleste-520EX_aleste | 1.9054 → 2.013 | 0 → 1 | 22 → 7 | 101 → 60 | 512 → 443 | 1.2024 → 1.2062 | 54 → 88 |
| DSP-ADAU1452_DSP-ADAU1452 | 0.3981 → 0.3981 | 0 → 0 | 2 → 2 | 313 → 312 | 304 → 304 | 1.0406 → 1.041 | 9 → 9 |
| Horticulture_Light_EQ_3-Band_light | 0.5547 → 0.5648 | 0 → 0 | 1 → 1 | 325 → 390 | 59 → 66 | 1.1808 → 1.0281 | 7 → 1 |
| Keyboard_PCB_Keyboard | 0.9135 → 0.9229 | 0 → 0 | 0 → 0 | 34 → 36 | 112 → 111 | 1.0251 → 1.0264 | 4 → 4 |
| LeeChee_1800 | 1.011 → 1.0117 | 0 → 1 | 3 → 3 | 291 → 281 | 378 → 390 | 1.1808 → 1.1912 | 44 → 39 |
| TCKB_Kicad_TCKB | 1.1597 → 0.7045 | 0 → 0 | 5 → 20 | 94 → 350 | 284 → 1115 | 1.0605 → 1.048 | 5 → 2 |
| m2-electronics_m2fc | 0.5972 → 0.5344 | 0 → 0 | 7 → 1 | 154 → 66 | 234 → 774 | 1.0785 → 1.0708 | 27 → 27 |
| memsarray_mems_array | 1.0235 → 0.9948 | 0 → 0 | 0 → 1 | 41 → 40 | 124 → 126 | 1.0216 → 1.0215 | 6 → 6 |
| newer-motor-controllers_busparts | 0.5166 → 0.4928 | 1 → 0 | 0 → 0 | 58 → 61 | 96 → 103 | 1.0221 → 1.0241 | 2 → 2 |
| newer-motor-controllers_si31-3 | 0.4919 → 0.4929 | 0 → 0 | 0 → 0 | 58 → 56 | 109 → 108 | 1.0277 → 1.0279 | 3 → 3 |
| opendous_Upconverter | 0.2891 → 0.2881 | 0 → 0 | 2 → 2 | 332 → 331 | 64 → 65 | 1.0691 → 1.0691 | 5 → 5 |

Comparison notes (not a gate; CPU speed needs equal wall time, doc 10 §5):
- Aleste-520EX_aleste: completion uses KiCad's capped unconnected list, not exact connectivity
- DSP-ADAU1452_DSP-ADAU1452: completion uses KiCad's capped unconnected list, not exact connectivity
- EUC-VESC_electronics_sept: completion uses KiCad's capped unconnected list, not exact connectivity
- Hardware_Playground_heater_actuator_node: completion uses KiCad's capped unconnected list, not exact connectivity
- Keyboard_PCB_Keyboard: completion uses KiCad's capped unconnected list, not exact connectivity
- LeeChee_1800: completion uses KiCad's capped unconnected list, not exact connectivity
- TCKB_Kicad_TCKB: completion uses KiCad's capped unconnected list, not exact connectivity
- m2-electronics_m2fc: completion uses KiCad's capped unconnected list, not exact connectivity
- opendous_Upconverter: completion uses KiCad's capped unconnected list, not exact connectivity
- perfplusplus_Ard-perf++: completion uses KiCad's capped unconnected list, not exact connectivity

Gates: FAIL
- CoreOne-xCORE200-Original_CoreOne: judge failed (before)
- Hardware_Playground_heater_actuator_node: before evaluated DRC errors are capped or unavailable; gate unknown
- Hardware_Playground_heater_actuator_node: after evaluated DRC errors are capped or unavailable; gate unknown
- KiCad-Library_Teensy_test_layout: judge failed (before)
- LeeChee_1800: before evaluated DRC errors are capped or unavailable; gate unknown
- LeeChee_1800: after evaluated DRC errors are capped or unavailable; gate unknown
- LeeChee_1800: worse (clean False -> False, completion 100.0% -> 99.8%)
- MonApollo_analog-board: judge failed (before)
- TCKB_Kicad_TCKB: worse (clean False -> False, completion 95.8% -> 95.6%)
- badge2016_Badge_init: before evaluated DRC errors are capped or unavailable; gate unknown
- badge2016_Badge_init: after evaluated DRC errors are capped or unavailable; gate unknown
- dorkyboard_keyboard: judge failed (before)
- front-end-modules_LimeSDR_Sony: judge failed (before)
- m2-electronics_m2fc: worse (clean True -> False, completion 100.0% -> 99.8%)
- mechkeys_LFK78: judge failed (after)
- mini_mass_prog_bench_prog_rig: judge failed (before)
- perfplusplus_Ard-perf++: before evaluated DRC errors are capped or unavailable; gate unknown
- perfplusplus_Ard-perf++: after evaluated DRC errors are capped or unavailable; gate unknown
