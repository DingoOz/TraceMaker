# e1-base → e1-pw05

30 boards; routed boards byte-identical: 4/30.
Clean pass 21 → 21; mean completion 98.3% → 98.2%; router seconds 471.3 → 457.7.
Route CPU time 2,690 → 2,611 s (-2.9%) at the same work.
Byte-identical boards moved -21% to +3% in CPU time (the same work: noise, or speed).

| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |
|---|---|---|---|---|---|---|---|
| CPU-Power-Supply-Mod_CPU_Power_Supply_Mod | 135 → 135 / 135 | 100.0% → 100.0% | yes → yes | – | 9.1 → 9.1 | 59.4 → 59.3 | differs |
| DustSensorShield_DustSensorShield | 17 → 17 / 17 | 100.0% → 100.0% | yes → yes | – | 7.8 → 7.9 | 47.5 → 47.1 | differs |
| E202VAR-Natural-Radio-Receiver_e202var-vlf-radio-receiver | 92 → 92 / 92 | 100.0% → 100.0% | yes → yes | – | 9.0 → 8.9 | 36.4 → 36.8 | differs |
| EUC-VESC_electronics_BJT | 361 → 361 / 366 | 98.6% → 98.6% | no → no | – | 37.7 → 36.9 | 240.4 → 234.2 | differs |
| ErgodoneWithHole_ErgoDone | 352 → 353 / 356 | 98.9% → 99.2% | no → no | – | 40.8 → 36.0 | 220.0 → 202.2 | differs |
| HW-ESP8266_Roomba_ESP8266_Roomba_panel_x_3 | 173 → 173 / 181 | 95.6% → 95.6% | no → no | – | 30.3 → 33.4 | 186.0 → 196.9 | differs |
| Hangul-Clock_Hangul | 209 → 209 / 209 | 100.0% → 100.0% | yes → yes | – | 13.2 → 13.6 | 77.5 → 78.8 | differs |
| Hardware_Playground_pro_mini | 78 → 78 / 78 | 100.0% → 100.0% | yes → yes | – | 5.2 → 5.8 | 33.2 → 33.8 | differs |
| Hardware_Playground_rpi_zero_ws2812 | 46 → 46 / 46 | 100.0% → 100.0% | yes → yes | – | 2.1 → 3.6 | 9.4 → 14.8 | differs |
| Horticulture_Light_EQ_3-Band_3 channel led driver rev0.4 | 368 → 368 / 368 | 100.0% → 100.0% | yes → yes | – | 7.9 → 7.8 | 47.2 → 43.7 | differs |
| LadybugLiteBlue_HW_LadybugBlueLite | 140 → 140 / 140 | 100.0% → 100.0% | yes → yes | – | 8.7 → 7.0 | 45.2 → 42.7 | differs |
| NeoWall_NeoWall | 32 → 32 / 32 | 100.0% → 100.0% | yes → yes | – | 4.5 → 4.2 | 18.9 → 20.4 | differs |
| SPEX_HAB_Mainboard_Hardware_Power | 231 → 227 / 259 | 89.2% → 87.6% | no → no | – | 35.0 → 34.6 | 197.0 → 200.8 | differs |
| SmartLaserCO2-PCB_WaterCool | 8 → 8 / 8 | 100.0% → 100.0% | yes → yes | – | 0.5 → 0.5 | 2.1 → 2.5 | differs |
| Teensy-Hats_Temperature-Monitor-Hat | 29 → 29 / 29 | 100.0% → 100.0% | yes → yes | – | 12.5 → 10.4 | 73.2 → 62.4 | differs |
| hardware-designs_nixie-combo | 101 → 101 / 101 | 100.0% → 100.0% | yes → yes | – | 12.6 → 12.7 | 79.8 → 81.8 | differs |
| kitspace_12_24_boost_converter | 31 → 31 / 31 | 100.0% → 100.0% | yes → yes | – | 1.3 → 1.2 | 4.4 → 3.5 | differs |
| kitspace_8_switch_array | 54 → 54 / 54 | 100.0% → 100.0% | yes → yes | – | 2.4 → 2.8 | 7.5 → 8.5 | differs |
| kitspace_OtterScreen | 121 → 121 / 121 | 100.0% → 100.0% | yes → yes | – | 10.8 → 11.2 | 66.7 → 66.5 | differs |
| kitspace_Unifying | 152 → 151 / 167 | 91.0% → 90.4% | no → no | – | 43.4 → 43.5 | 237.1 → 237.3 | differs |
| kitspace_esp8266 | 53 → 53 / 53 | 100.0% → 100.0% | yes → yes | – | 10.4 → 9.1 | 55.8 → 50.2 | differs |
| open-source-multimeter_board | 87 → 87 / 108 | 80.6% → 80.6% | no → no | – | 34.2 → 37.1 | 191.7 → 186.9 | differs |
| solar-lora_solar-lora | 130 → 130 / 136 | 95.6% → 95.6% | no → no | – | 25.1 → 23.2 | 154.9 → 143.8 | differs |
| temperature-alarm_controller | 53 → 53 / 53 | 100.0% → 100.0% | yes → yes | – | 9.3 → 9.4 | 44.9 → 37.5 | differs |
| timecircuits-hardware_BTTF-TimeCircuits | 443 → 445 / 446 | 99.3% → 99.8% | no → no | – | 23.5 → 23.8 | 146.0 → 148.0 | differs |
| tinyDriverP_tinybot | 73 → 73 / 73 | 100.0% → 100.0% | yes → yes | – | 12.8 → 7.0 | 74.5 → 42.6 | differs |

Style (informational): bends per connection 0.8467 → 0.8295; acute angles 0 → 2; stair steps 10 → 20; short segments 610 → 947; off angle segments 1214 → 1354; detour ratio 1.0787 → 1.0833; long detours 99 → 105.

| Board | Bends per connection | Acute angles | Stair steps | Short segments | Off angle segments | Detour ratio | Long detours |
|---|---|---|---|---|---|---|---|
| CPU-Power-Supply-Mod_CPU_Power_Supply_Mod | 0.8075 → 0.8438 | 0 → 0 | 0 → 0 | 6 → 12 | 8 → 6 | 1.0294 → 1.032 | 0 → 0 |
| DustSensorShield_DustSensorShield | 1.5 → 1.5 | 0 → 0 | 0 → 0 | 1 → 3 | 3 → 3 | 1.0495 → 1.0495 | 1 → 1 |
| E202VAR-Natural-Radio-Receiver_e202var-vlf-radio-receiver | 0.6718 → 0.6617 | 0 → 0 | 0 → 0 | 6 → 8 | 18 → 20 | 1.0287 → 1.0284 | 0 → 0 |
| EUC-VESC_electronics_BJT | 0.4229 → 0.4213 | 0 → 0 | 1 → 2 | 127 → 138 | 162 → 162 | 1.0488 → 1.0488 | 7 → 7 |
| ErgodoneWithHole_ErgoDone | 0.745 → 0.7587 | 0 → 0 | 0 → 0 | 19 → 19 | 140 → 135 | 1.0789 → 1.0793 | 3 → 3 |
| HW-ESP8266_Roomba_ESP8266_Roomba_panel_x_3 | 0.9208 → 0.8841 | 0 → 0 | 0 → 0 | 37 → 84 | 61 → 74 | 1.2194 → 1.2045 | 14 → 13 |
| Hangul-Clock_Hangul | 1.1784 → 1.0646 | 0 → 0 | 0 → 0 | 6 → 22 | 59 → 84 | 1.0343 → 1.0388 | 4 → 4 |
| Hardware_Playground_pro_mini | 0.9273 → 0.7273 | 0 → 0 | 1 → 0 | 19 → 23 | 12 → 17 | 1.2611 → 1.1617 | 5 → 5 |
| Hardware_Playground_rpi_zero_ws2812 | 0.6935 → 0.697 | 0 → 0 | 0 → 0 | 9 → 10 | 2 → 6 | 1.014 → 1.0919 | 0 → 2 |
| Horticulture_Light_EQ_3-Band_3 channel led driver rev0.4 | 0.3597 → 0.3465 | 0 → 0 | 3 → 3 | 67 → 71 | 64 → 61 | 1.1105 → 1.1308 | 5 → 5 |
| LadybugLiteBlue_HW_LadybugBlueLite | 0.444 → 0.4453 | 0 → 0 | 0 → 0 | 30 → 56 | 94 → 89 | 1.0416 → 1.0384 | 2 → 2 |
| NeoWall_NeoWall | 0.4286 → 0.4286 | 0 → 0 | 0 → 0 | 0 → 4 | 24 → 24 | 1.0009 → 1.0012 | 0 → 0 |
| SPEX_HAB_Mainboard_Hardware_Power | 0.7368 → 0.7809 | 0 → 0 | 0 → 2 | 21 → 68 | 65 → 71 | 1.0765 → 1.1884 | 5 → 8 |
| Teensy-Hats_Temperature-Monitor-Hat | 1.2 → 1.4615 | 0 → 0 | 0 → 0 | 3 → 6 | 9 → 7 | 1.0337 → 1.0749 | 0 → 1 |
| hardware-designs_nixie-combo | 1.4451 → 1.5438 | 0 → 0 | 0 → 0 | 6 → 9 | 52 → 51 | 1.0364 → 1.0406 | 0 → 0 |
| kitspace_12_24_boost_converter | 0.7949 → 0.775 | 0 → 0 | 0 → 0 | 1 → 1 | 1 → 2 | 1.0224 → 1.0272 | 0 → 0 |
| kitspace_8_switch_array | 0.9048 → 0.9048 | 0 → 0 | 0 → 0 | 2 → 2 | 4 → 4 | 1.0242 → 1.0302 | 0 → 0 |
| kitspace_OtterScreen | 0.6875 → 0.7044 | 0 → 0 | 0 → 0 | 14 → 46 | 52 → 54 | 1.1024 → 1.0673 | 8 → 10 |
| kitspace_Unifying | 1.5302 → 1.4779 | 0 → 0 | 2 → 1 | 50 → 53 | 45 → 39 | 1.154 → 1.1289 | 12 → 11 |
| kitspace_esp8266 | 1.0484 → 1.0448 | 0 → 0 | 0 → 0 | 0 → 2 | 4 → 4 | 1.0754 → 1.0878 | 0 → 0 |
| open-source-multimeter_board | 1.1947 → 1.031 | 0 → 2 | 0 → 8 | 5 → 84 | 12 → 169 | 1.0664 → 1.0597 | 4 → 4 |
| solar-lora_solar-lora | 1.2019 → 1.1743 | 0 → 0 | 2 → 2 | 34 → 50 | 51 → 51 | 1.2892 → 1.2846 | 16 → 15 |
| temperature-alarm_controller | 0.7887 → 0.6923 | 0 → 0 | 0 → 0 | 6 → 7 | 8 → 14 | 1.0458 → 1.055 | 1 → 1 |
| timecircuits-hardware_BTTF-TimeCircuits | 1.3024 → 1.2361 | 0 → 0 | 0 → 0 | 51 → 70 | 79 → 69 | 1.0892 → 1.0907 | 7 → 8 |
| tinyDriverP_tinybot | 1.2479 → 1.0149 | 0 → 0 | 0 → 1 | 29 → 38 | 87 → 40 | 1.1272 → 1.1957 | 4 → 4 |

Comparison notes (not a gate; CPU speed needs equal wall time, doc 10 §5):
- Hardware_Playground_rpi_zero_ws2812: route CPU time 9.4 → 14.8 s (+58%)
- kitspace_12_24_boost_converter: route CPU time 4.4 → 3.5 s (-22%)
- tinyDriverP_tinybot: route CPU time 74.5 → 42.6 s (-43%)

Gates: FAIL
- ErgodoneWithHole_ErgoDone: before evaluated DRC errors are capped or unavailable; gate unknown
- ErgodoneWithHole_ErgoDone: after evaluated DRC errors are capped or unavailable; gate unknown
- SPEX_HAB_Mainboard_Hardware_Power: before evaluated DRC errors are capped or unavailable; gate unknown
- SPEX_HAB_Mainboard_Hardware_Power: after evaluated DRC errors are capped or unavailable; gate unknown
- SPEX_HAB_Mainboard_Hardware_Power: worse (clean False -> False, completion 89.2% -> 87.6%)
- kitspace_Unifying: worse (clean False -> False, completion 91.0% -> 90.4%)
- timecircuits-hardware_BTTF-TimeCircuits: before evaluated DRC errors are capped or unavailable; gate unknown
- timecircuits-hardware_BTTF-TimeCircuits: after evaluated DRC errors are capped or unavailable; gate unknown
