# thin-quick-main → rescue-quick

30 boards; routed boards byte-identical: 22/30.
Clean pass 11 → 12; mean completion 89.8% → 90.1%; router seconds 61.9 → 70.9.
Route CPU time 85 → 91 s (+7.2%) at the same work.
Byte-identical boards moved -47% to +86% in CPU time (the same work: noise, or speed).

| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |
|---|---|---|---|---|---|---|---|
| C-BISCUIT_buck-reg-5v | 54 → 55 / 55 | 98.2% → 100.0% | no → yes | – | 3.0 → 2.1 | 3.9 → 2.8 | differs |
| Hangul-Clock_Hangul | 147 → 152 / 209 | 70.3% → 72.7% | no → no | – | 2.3 → 2.8 | 3.0 → 3.2 | differs |
| Inkjet__autosave-InkjetDriver | 49 → 49 / 65 | 75.4% → 75.4% | no → no | – | 2.3 → 2.5 | 3.0 → 3.0 | differs |
| LogicBoxen_LogicBoxen | 78 → 80 / 116 | 67.2% → 69.0% | no → no | – | 1.9 → 1.8 | 2.6 → 2.3 | differs |
| custom_cpu--ALU_custom_cpu--ALU | 57 → 57 / 83 | 68.7% → 68.7% | no → no | – | 2.6 → 5.2 | 3.3 → 5.8 | differs |
| fan_controller_fan_controller | 99 → 101 / 115 | 86.1% → 87.8% | no → no | – | 1.5 → 3.1 | 2.3 → 3.9 | differs |
| komputer-klavier_KomputerKlavier | 82 → 82 / 87 | 94.2% → 94.2% | no → no | – | 3.0 → 4.5 | 3.5 → 5.1 | differs |
| uC3Moy_uC3Moy | 39 → 40 / 41 | 95.1% → 97.6% | no → no | – | 2.4 → 2.9 | 3.1 → 3.5 | differs |

Style (informational): bends per connection 0.6303 → 0.6221; acute angles 0 → 0; stair steps 30 → 29; short segments 268 → 268; off angle segments 417 → 688; detour ratio 1.032 → 1.0309; long detours 25 → 24.

| Board | Bends per connection | Acute angles | Stair steps | Short segments | Off angle segments | Detour ratio | Long detours |
|---|---|---|---|---|---|---|---|
| C-BISCUIT_buck-reg-5v | 0.5195 → 0.5063 | 0 → 0 | 0 → 0 | 9 → 7 | 8 → 22 | 1.0445 → 1.0286 | 1 → 0 |
| Hangul-Clock_Hangul | 0.8024 → 0.7697 | 0 → 0 | 0 → 0 | 0 → 0 | 16 → 25 | 1.0 → 0.9996 | 0 → 0 |
| Inkjet__autosave-InkjetDriver | 0.5432 → 0.6301 | 0 → 0 | 0 → 0 | 5 → 6 | 28 → 57 | 1.0153 → 1.0124 | 0 → 0 |
| LogicBoxen_LogicBoxen | 0.3712 → 0.3217 | 0 → 0 | 0 → 0 | 30 → 29 | 24 → 95 | 1.09 → 1.0748 | 3 → 3 |
| custom_cpu--ALU_custom_cpu--ALU | 1.0667 → 0.9677 | 0 → 0 | 0 → 0 | 4 → 4 | 3 → 8 | 1.0216 → 1.0159 | 0 → 0 |
| fan_controller_fan_controller | 0.7206 → 0.5906 | 0 → 0 | 1 → 0 | 14 → 15 | 20 → 86 | 1.0524 → 1.0436 | 2 → 2 |
| komputer-klavier_KomputerKlavier | 0.5607 → 0.581 | 0 → 0 | 0 → 0 | 2 → 2 | 3 → 38 | 1.0205 → 1.0202 | 0 → 0 |
| uC3Moy_uC3Moy | 0.7708 → 0.9583 | 0 → 0 | 0 → 0 | 1 → 2 | 4 → 46 | 1.0261 → 1.0477 | 0 → 0 |

Gates: pass
