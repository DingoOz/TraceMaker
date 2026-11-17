# M24: moving parts a hand placement has pinned down (D192, doc 04 §12), 2026-11-17

Engine: branch `m26-impl` (built on 9b77616 plus the M24 changes). Board set and recipe as D190
(`bench/records/m24-closure`): `bench/run.py --board-list bench/records/m24-closure/closure-eval.txt --place closure
--place-work 10000000 --place-timeout 3000 --work 10000000 --jobs 7 --place-args="--closure-keep-overlaps"`. KiCad 10 judges.

| Run | Placer |
|---|---|
| `m24-keep10m` (a) | exact courtyards; keep rule: any pair the placer's model reports in the input may stay |
| `m24-keep10m-b` (b) | (a), plus: only pairs whose courtyards really overlap may stay, their holes and copper must stay clear; Margin pieces hold pads off like the edge |
| `m24-keep10m-c` (c) | (b), plus moves legal only by the keep rule rank after every strictly legal move of their arm (the version committed) |

`placed/` holds run (c)'s placer reports. `added_errors.py` lists the placement-added KiCad errors of a board (pair judge).

## Results

| Run | Open | Complete | Clean |
|---|--:|--:|--:|
| D190, `closure-route10m` | 32 | 4 | 3 |
| (a) | 30 | 5 | 3* |
| (b) | 31 | 5 | 4 |
| (c) | 32 | 4 | 3 |

\* (a) made 1Bitsy and phone_amp gain real KiCad errors (below), so (a) is not a valid result.

Open connections per board (KiCad), "clean" when complete and no error added:

| Board | D190 | a | b | c | c gate (position / pair) |
|---|--:|--:|--:|--:|---|
| Hardware_Playground_serial_gw_ATMEGA328P | 1 | 0 clean | 0 clean | 0 clean | pass / pass |
| front-end-modules_LimeSDR_Sony | 0 | 1 | 0 | 0 | unknown / unknown |
| memsarray_mems_modules | 1 | 1 | 1 | 1 | pass / pass |
| ErgodoneWithHole_ErgoDone | 2 | 2 | 2 | 2 | unknown / unknown |
| Hardware_Playground_sd_wav_pcm5100 | 2 | 3 | 2 | 2 | pass / pass |
| 1Bitsy_1bitsy | 0 clean | 0 | 0 clean | 0 clean | pass / pass |
| Hardware_Playground_Touch_Switch_2ch_PCB | 0 clean | 0 clean | 0 clean | 0 clean | pass / pass |
| 96boards-robomezzi_96boards-robomezzi | 2 | 2 | 2 | 2 | pass / pass |
| kitspace_teensy-fx | 1 | 1 | 1 | 1 | pass / pass |
| mechkeys_58r-mxalps | 0 clean | 0 clean | 2 | 2 | pass / pass |
| motor-high-current-sensors_CurrentSensorPCB_line | 1 | 1 | 0 clean | 1 | pass / pass |
| kitspace_ardfpga | 5 | 5 | 5 | 5 | pass / pass |
| phone_amp_phone_amp | 5 | 2 | 4 | 4 | fail / pass |
| bullion_bullion | 12 | 12 | 12 | 12 | pass / pass |

- **serial_gw is now clean in every run.** Its one move (IC1 1 mm in −y; C1-C7 sit under the RFM69 module's back courtyard) was not
  legal under D190's rule.
- **phone_amp gets moves** (37 trials, C3 0.5 mm kept): 5 → 4 open. KiCad's position-matched gate fails on it because C3 has a malformed
  courtyard in the library footprint and moving C3 moves that error; matched by footprint (the pair judge) nothing is added.
- **memsarray never lacked candidates.** D190's run also tried 34 moves; with 2 movable parts of 6 it exhausts them. The roadmap's
  "no candidates" was the stop reason after exhaustion, not an empty first list.
- **58r-mxalps is lost in (b) and (c).** A room move (C5, C7 pushed 0.5 mm) is legal now and the bandit pulls it early; the run
  then reaches its 1,800 s loop limit at trial 15 before D190's completing move (PROGRAM 0.5 mm, its 11th trial) comes up.
- **Run (a)'s errors** and their fixes: on phone_amp C3 moved onto a pad of U1 (a short), because a kept pair skipped the copper test;
  on 1Bitsy X3 moved next to C18, a pair the model counted only because their courtyards were within the courtyard clearance, and
  KiCad reported a new overlap and a pad clearance. Run (a) also moved pads beside phone_amp's Margin lines, which the placer did not know.

## Gate (doc 11 M24)

Not met. The three boards get legal moves (serial_gw completes clean, phone_amp 5 → 4, memsarray exhausts its moves as before), and no
board gains a KiCad error from placement when footprint errors are matched by footprint. But 58r-mxalps is worse than D190's run at equal
routes, and phone_amp fails the position-matched gate. `--closure-keep-overlaps` therefore stays off by default.
