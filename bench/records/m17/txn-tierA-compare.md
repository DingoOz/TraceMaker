# txn-base-tierA → txn-txn-tierA

40 boards; routed boards byte-identical: 33/40.
Clean pass 40 → 40; mean completion 100.0% → 100.0%; router seconds 781.3 → 694.5.
Route CPU time 3,073 → 2,896 s (-5.7%).

| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |
|---|---|---|---|---|---|---|---|
| 655_testboard | 57 → 57 / 57 | 100.0% → 100.0% | yes → yes | – | 29.0 → 21.8 | 120.1 → 89.3 | differs |
| Hangul-Clock_Hangul | 209 → 209 / 209 | 100.0% → 100.0% | yes → yes | – | 58.4 → 56.9 | 333.5 → 325.2 | differs |
| LogicBoxen_LogicBoxen | 116 → 116 / 116 | 100.0% → 100.0% | yes → yes | – | 70.7 → 67.0 | 252.6 → 241.5 | differs |
| esp-serial-terminal_esp-com | 59 → 59 / 59 | 100.0% → 100.0% | yes → yes | – | 9.3 → 9.3 | 44.4 → 42.5 | differs |
| kitspace_aquarius | 171 → 171 / 171 | 100.0% → 100.0% | yes → yes | – | 120.9 → 49.3 | 303.9 → 196.1 | differs |
| mosavr_pcie1x_backplane | 110 → 110 / 110 | 100.0% → 100.0% | yes → yes | – | 42.1 → 47.4 | 150.2 → 145.7 | differs |
| uC3Moy_uC3Moy | 41 → 41 / 41 | 100.0% → 100.0% | yes → yes | – | 6.5 → 6.7 | 31.9 → 36.2 | differs |

Style (informational): bends per connection 0.8351 → 0.8435; acute angles 0 → 0; stair steps 35 → 34; short segments 442 → 448; off angle segments 702 → 727; detour ratio 1.0591 → 1.0598; long detours 51 → 54.

| Board | Bends per connection | Acute angles | Stair steps | Short segments | Off angle segments | Detour ratio | Long detours |
|---|---|---|---|---|---|---|---|
| 655_testboard | 1.5914 → 1.4792 | 0 → 0 | 2 → 1 | 18 → 18 | 19 → 18 | 1.2144 → 1.2105 | 8 → 6 |
| Hangul-Clock_Hangul | 1.2 → 1.1699 | 0 → 0 | 0 → 0 | 4 → 5 | 44 → 47 | 1.022 → 1.0209 | 0 → 0 |
| LogicBoxen_LogicBoxen | 0.7635 → 0.9568 | 0 → 0 | 0 → 0 | 44 → 42 | 51 → 42 | 1.1311 → 1.1507 | 3 → 5 |
| esp-serial-terminal_esp-com | 0.6146 → 0.6311 | 0 → 0 | 0 → 0 | 7 → 10 | 19 → 28 | 1.0602 → 1.053 | 2 → 1 |
| kitspace_aquarius | 0.7786 → 0.927 | 0 → 0 | 1 → 1 | 30 → 29 | 69 → 67 | 1.0703 → 1.0933 | 9 → 12 |
| mosavr_pcie1x_backplane | 1.5355 → 1.3675 | 0 → 0 | 27 → 27 | 44 → 50 | 17 → 38 | 1.1184 → 1.0855 | 3 → 4 |
| uC3Moy_uC3Moy | 1.14 → 0.9077 | 0 → 0 | 0 → 0 | 2 → 1 | 3 → 7 | 1.0766 → 1.0861 | 0 → 0 |

Gates: pass
