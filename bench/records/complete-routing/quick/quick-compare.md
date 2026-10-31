# rescue-quick → cr-quick2

30 boards; routed boards byte-identical: 12/30.
Clean pass 12 → 16; mean completion 90.1% → 93.9%; router seconds 70.9 → 66.7.
Route CPU time 91 → 94 s (+4.2%) at the same work.
Byte-identical boards moved -37% to +32% in CPU time (the same work: noise, or speed).

| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |
|---|---|---|---|---|---|---|---|
| 655_testboard | 41 → 44 / 57 | 71.9% → 77.2% | no → no | – | 1.6 → 2.4 | 2.5 → 3.8 | differs |
| C-BISCUIT_buck-reg-5v | 55 → 55 / 55 | 100.0% → 100.0% | yes → yes | – | 2.1 → 1.7 | 2.8 → 2.5 | differs |
| ESP8266-MQTT-battery-monitor-hw_battery-monitor | 78 → 90 / 95 | 82.1% → 94.7% | no → no | – | 1.7 → 2.7 | 2.4 → 4.0 | differs |
| Hangul-Clock_Hangul | 152 → 161 / 209 | 72.7% → 77.0% | no → no | – | 2.8 → 3.6 | 3.2 → 6.9 | differs |
| Inkjet__autosave-InkjetDriver | 49 → 53 / 65 | 75.4% → 81.5% | no → no | – | 2.5 → 3.9 | 3.0 → 4.7 | differs |
| LogicBoxen_LogicBoxen | 80 → 91 / 116 | 69.0% → 78.5% | no → no | – | 1.8 → 2.8 | 2.3 → 4.3 | differs |
| atmel-programmer_atmel_programmer | 27 → 32 / 39 | 69.2% → 82.0% | no → no | – | 5.7 → 5.3 | 6.3 → 6.0 | differs |
| breakout-boards_swd-and-uart | 9 → 11 / 11 | 81.8% → 100.0% | no → yes | – | 2.6 → 1.8 | 3.4 → 2.5 | differs |
| custom_cpu--ALU_custom_cpu--ALU | 57 → 62 / 83 | 68.7% → 74.7% | no → no | – | 5.2 → 4.3 | 5.8 → 5.6 | differs |
| esp-serial-terminal_esp-com | 57 → 59 / 59 | 96.6% → 100.0% | no → yes | – | 2.3 → 2.1 | 2.9 → 2.6 | differs |
| esp8266_envmonitor_environment-monitor-1.2 | 31 → 32 / 32 | 96.9% → 100.0% | no → yes | – | 3.6 → 1.9 | 4.3 → 2.4 | differs |
| fan_controller_fan_controller | 101 → 109 / 115 | 87.8% → 94.8% | no → no | – | 3.1 → 3.0 | 3.9 → 3.8 | differs |
| kitspace_threeboard | 98 → 107 / 119 | 82.3% → 89.9% | no → no | – | 2.3 → 3.1 | 3.0 → 4.3 | differs |
| komputer-klavier_KomputerKlavier | 82 → 86 / 87 | 94.2% → 98.9% | no → no | – | 4.5 → 4.0 | 5.1 → 4.7 | differs |
| mechkeys_1800-controller | 137 → 145 / 166 | 82.5% → 87.4% | no → no | – | 4.5 → 4.0 | 5.3 → 5.4 | differs |
| mosavr_pcie1x_backplane | 91 → 94 / 110 | 82.7% → 85.5% | no → no | – | 4.2 → 3.6 | 4.8 → 4.7 | differs |
| scimpy_volumebuffer | 64 → 66 / 67 | 95.5% → 98.5% | no → no | – | 3.7 → 3.1 | 4.3 → 3.9 | differs |
| spisolator_spisolator | 30 → 31 / 31 | 96.8% → 100.0% | no → yes | – | 2.5 → 1.5 | 3.1 → 2.0 | differs |

Style (informational): bends per connection 0.6221 → 0.6694; acute angles 0 → 0; stair steps 29 → 29; short segments 268 → 276; off angle segments 688 → 669; detour ratio 1.0309 → 1.0388; long detours 24 → 29.

| Board | Bends per connection | Acute angles | Stair steps | Short segments | Off angle segments | Detour ratio | Long detours |
|---|---|---|---|---|---|---|---|
| 655_testboard | 1.0308 → 1.0423 | 0 → 0 | 1 → 1 | 12 → 12 | 14 → 17 | 1.1006 → 1.1343 | 3 → 4 |
| C-BISCUIT_buck-reg-5v | 0.5063 → 0.5 | 0 → 0 | 0 → 0 | 7 → 8 | 22 → 10 | 1.0286 → 1.0456 | 0 → 1 |
| ESP8266-MQTT-battery-monitor-hw_battery-monitor | 0.2464 → 0.2875 | 0 → 0 | 0 → 0 | 22 → 25 | 38 → 57 | 1.01 → 1.0108 | 0 → 0 |
| Hangul-Clock_Hangul | 0.7697 → 0.7644 | 0 → 0 | 0 → 0 | 0 → 0 | 25 → 26 | 0.9996 → 1.0037 | 0 → 0 |
| Inkjet__autosave-InkjetDriver | 0.6301 → 0.6627 | 0 → 0 | 0 → 0 | 6 → 6 | 57 → 39 | 1.0124 → 1.0308 | 0 → 0 |
| LogicBoxen_LogicBoxen | 0.3217 → 0.4 | 0 → 0 | 0 → 0 | 29 → 34 | 95 → 55 | 1.0748 → 1.0768 | 3 → 3 |
| atmel-programmer_atmel_programmer | 1.125 → 1.2895 | 0 → 0 | 0 → 0 | 1 → 1 | 4 → 11 | 1.0138 → 1.0162 | 0 → 0 |
| breakout-boards_swd-and-uart | 1.5455 → 1.6429 | 0 → 0 | 0 → 0 | 0 → 0 | 2 → 3 | 1.22 → 1.3085 | 2 → 3 |
| custom_cpu--ALU_custom_cpu--ALU | 0.9677 → 1.087 | 0 → 0 | 0 → 0 | 4 → 4 | 8 → 7 | 1.0159 → 1.0217 | 0 → 0 |
| esp-serial-terminal_esp-com | 0.5 → 0.5408 | 0 → 0 | 0 → 0 | 7 → 8 | 26 → 46 | 1.0382 → 1.0493 | 0 → 0 |
| esp8266_envmonitor_environment-monitor-1.2 | 0.5532 → 0.5625 | 0 → 0 | 0 → 0 | 1 → 0 | 10 → 9 | 1.0326 → 1.0292 | 1 → 1 |
| fan_controller_fan_controller | 0.5906 → 0.7372 | 0 → 0 | 0 → 1 | 15 → 17 | 86 → 55 | 1.0436 → 1.0618 | 2 → 2 |
| kitspace_threeboard | 0.6906 → 0.7044 | 0 → 0 | 0 → 0 | 5 → 5 | 23 → 60 | 1.0673 → 1.072 | 2 → 2 |
| komputer-klavier_KomputerKlavier | 0.581 → 0.6726 | 0 → 0 | 0 → 0 | 2 → 2 | 38 → 19 | 1.0202 → 1.0221 | 0 → 0 |
| mechkeys_1800-controller | 0.4128 → 0.4298 | 0 → 0 | 0 → 0 | 10 → 13 | 49 → 82 | 1.0353 → 1.0393 | 4 → 5 |
| mosavr_pcie1x_backplane | 1.2937 → 1.2824 | 0 → 0 | 27 → 27 | 43 → 43 | 28 → 34 | 1.032 → 1.0309 | 1 → 1 |
| scimpy_volumebuffer | 0.8706 → 1.0805 | 0 → 0 | 0 → 0 | 7 → 8 | 7 → 7 | 1.0388 → 1.0549 | 0 → 0 |
| spisolator_spisolator | 0.5333 → 0.9153 | 0 → 0 | 1 → 0 | 12 → 5 | 60 → 36 | 1.1439 → 1.2341 | 3 → 4 |

Comparison notes (not a gate; CPU speed needs equal wall time, doc 10 §5):
- 655_testboard: route CPU time 2.5 → 3.8 s (+54%)
- ESP8266-MQTT-battery-monitor-hw_battery-monitor: route CPU time 2.4 → 4.0 s (+72%)
- Hangul-Clock_Hangul: route CPU time 3.2 → 6.9 s (+116%)
- Inkjet__autosave-InkjetDriver: route CPU time 3.0 → 4.7 s (+54%)
- LogicBoxen_LogicBoxen: route CPU time 2.3 → 4.3 s (+88%)
- esp8266_envmonitor_environment-monitor-1.2: route CPU time 4.3 → 2.4 s (-44%)
- kitspace_threeboard: route CPU time 3.0 → 4.3 s (+47%)

Gates: pass
