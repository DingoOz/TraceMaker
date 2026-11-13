# txn-base-tierC → txn-txn-tierC

30 boards; routed boards byte-identical: 8/30.
Clean pass 17 → 16; mean completion 99.3% → 99.7%; router seconds 2747.9 → 2269.1.

| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |
|---|---|---|---|---|---|---|---|
| AmpOne_dev-AmpOne | 321 → 321 / 321 | 100.0% → 100.0% | yes → yes | – | 88.2 → 94.1 | 586.9 → 497.8 | differs |
| Apple-M0110-BT_Apple M0110 | 224 → 224 / 224 | 100.0% → 100.0% | yes → yes | – | 45.6 → 54.8 | 205.3 → 176.6 | differs |
| Biscay_Blueeye_sipm | 306 → 306 / 306 | 100.0% → 100.0% | yes → yes | – | 120.4 → 120.6 | 564.4 → 665.7 | differs |
| Hardware_Playground_multisensor_cr2032 | 205 → 205 / 205 | 100.0% → 100.0% | yes → yes | – | 11.7 → 120.4 | 89.8 → 962.4 | differs |
| draco_draco | 418 → 418 / 418 | 100.0% → 100.0% | yes → yes | – | 121.0 → 120.5 | 748.7 → 796.7 | differs |
| jadonk_PocketBone | 202 → 202 / 202 | 100.0% → 100.0% | yes → yes | – | 121.0 → 122.1 | 690.0 → 792.5 | differs |
| kitspace_40-channel-hv-switching-board | 604 → 604 / 604 | 100.0% → 100.0% | yes → yes | – | 120.3 → 120.9 | 867.7 → 917.1 | differs |
| kitspace_PSLab | 339 → 339 / 339 | 100.0% → 100.0% | yes → yes | – | 121.4 → 121.3 | 579.5 → 684.2 | differs |
| kitspace_dropbot_control_board | 307 → 307 / 307 | 100.0% → 100.0% | yes → yes | – | 123.9 → 120.9 | 691.0 → 594.6 | differs |
| mechkeys_MF68+10 | 415 → 415 / 415 | 100.0% → 100.0% | yes → yes | – | 123.2 → 123.5 | 969.5 → 971.0 | differs |
| tiny-8088_Computer | 284 → 284 / 284 | 100.0% → 100.0% | yes → yes | – | 120.1 → 120.1 | 520.8 → 477.5 | differs |
| zx-sizif-512-ext_sizif512ext | 454 → 468 / 498 | 91.2% → 94.0% | no → no | – | 406.2 → 367.2 | 2779.5 → 2593.5 | differs |

Style (informational): bends per connection 0.8094 → 0.8126; acute angles 0 → 0; stair steps 27 → 24; short segments 1041 → 1153; off angle segments 3179 → 2799; detour ratio 1.0744 → 1.0832; long detours 132 → 144.

| Board | Bends per connection | Acute angles | Stair steps | Short segments | Off angle segments | Detour ratio | Long detours |
|---|---|---|---|---|---|---|---|
| AmpOne_dev-AmpOne | 0.7925 → 0.7304 | 0 → 0 | 1 → 1 | 28 → 28 | 55 → 66 | 1.0878 → 1.0756 | 24 → 17 |
| Apple-M0110-BT_Apple M0110 | 0.5895 → 0.5851 | 0 → 0 | 0 → 0 | 6 → 6 | 33 → 31 | 1.0095 → 1.0095 | 0 → 0 |
| Biscay_Blueeye_sipm | 0.6728 → 0.6911 | 0 → 0 | 3 → 3 | 52 → 54 | 128 → 128 | 1.0484 → 1.0518 | 11 → 12 |
| Hardware_Playground_multisensor_cr2032 | 0.5369 → 0.5349 | 0 → 0 | 1 → 0 | 38 → 41 | 59 → 63 | 1.0829 → 1.162 | 3 → 10 |
| draco_draco | 0.6871 → 0.6327 | 0 → 0 | 4 → 2 | 114 → 108 | 260 → 293 | 1.0683 → 1.0675 | 15 → 12 |
| jadonk_PocketBone | 1.0142 → 0.9268 | 0 → 0 | 0 → 0 | 31 → 31 | 113 → 118 | 1.0526 → 1.0408 | 3 → 2 |
| kitspace_40-channel-hv-switching-board | 0.6128 → 0.6176 | 0 → 0 | 1 → 1 | 110 → 117 | 440 → 436 | 1.0534 → 1.0544 | 5 → 5 |
| kitspace_PSLab | 0.6711 → 0.6884 | 0 → 0 | 3 → 2 | 93 → 87 | 282 → 279 | 1.0626 → 1.0616 | 9 → 13 |
| kitspace_dropbot_control_board | 0.5304 → 0.5169 | 0 → 0 | 0 → 0 | 80 → 86 | 145 → 158 | 1.059 → 1.0585 | 7 → 6 |
| mechkeys_MF68+10 | 1.5867 → 1.5985 | 0 → 0 | 3 → 5 | 87 → 90 | 146 → 145 | 1.0713 → 1.0737 | 5 → 4 |
| tiny-8088_Computer | 2.0578 → 2.0398 | 0 → 0 | 0 → 0 | 6 → 6 | 89 → 91 | 1.131 → 1.1403 | 4 → 5 |
| zx-sizif-512-ext_sizif512ext | 1.486 → 1.7537 | 0 → 0 | 2 → 3 | 64 → 74 | 194 → 246 | 1.1412 → 1.2138 | 15 → 24 |

Comparison notes (not a gate; CPU speed needs equal wall time, doc 10 §5):
- kitspace_40-channel-hv-switching-board: completion uses KiCad's capped unconnected list, not exact connectivity

Gates: FAIL
- 96boards-robomezzi_96boards-robomezzi: judge failed (before)
- DronPi_dronPi: judge failed (before)
- ErgodoneWithHole_ErgoDone: judge failed (after)
- Hardware_Playground_sd_wav_pcm5100: judge failed (before)
- Microdox-PCB_Microdox: before evaluated DRC errors are capped or unavailable; gate unknown
- Microdox-PCB_Microdox: after evaluated DRC errors are capped or unavailable; gate unknown
- Microdox-PCB_Microdox-topMCU: before evaluated DRC errors are capped or unavailable; gate unknown
- Microdox-PCB_Microdox-topMCU: after evaluated DRC errors are capped or unavailable; gate unknown
- P8000_WDC_Emulator_P8000_WDC_Emulator: judge failed (after)
- PCIE-to-MXM-Adapter_PCIEx1toMXM3.0: judge failed (before)
- kitspace_d20_tri_r0.3: judge failed (before)
- mechkeys_58r-mxalps: judge failed (after)
- motor-high-current-sensors_CurrentSensorPCB_line: judge failed (before)
- rvi_v2x_hardware_V2X: judge failed (before)
- wordclock_ledboard2: before evaluated DRC errors are capped or unavailable; gate unknown
- wordclock_ledboard2: after evaluated DRC errors are capped or unavailable; gate unknown
