| Board | closure-human10m | closure-human40m | closure-place10m | closure-route10m | closure-route10m-then40m |
|---|--:|--:|--:|--:|--:|
| 1Bitsy_1bitsy | 1 | 0 clean | 3 | 0 clean | 0 clean |
| 96boards-robomezzi_96boards-robomezzi | 2 | 2 | 1 | 2 | 2 |
| ErgodoneWithHole_ErgoDone | 3 | 2 | 2 | 2 | 2 |
| Hardware_Playground_Touch_Switch_2ch_PCB | 3 | 0 clean | 2 | 0 clean | 0 clean |
| Hardware_Playground_sd_wav_pcm5100 | 3 | 2 | 2 | 2 | 2 |
| Hardware_Playground_serial_gw_ATMEGA328P | 1 | 0 clean | 1 | 1 | 0 clean |
| bullion_bullion | 13 | 13 | 13 | 12 | 12 |
| front-end-modules_LimeSDR_Sony | 2 | 1 | 1 | 0 complete | 0 complete |
| kitspace_ardfpga | 7 | 4 | 4 | 5 | 5 |
| kitspace_teensy-fx | 5 | 2 | 2 | 1 | 1 |
| mechkeys_58r-mxalps | 6 | 4 | 7 | 0 clean | 0 clean |
| memsarray_mems_modules | 1 | 1 | 1 | 1 | 1 |
| motor-high-current-sensors_CurrentSensorPCB_line | 6 | 5 | 3 (+1 placement DRC) | 1 | 1 |
| phone_amp_phone_amp | 5 | 4 | 5 (+4 placement DRC) | 5 | 4 |
| **Total open / complete / clean** | 58 / 0 / 0 | 40 / 3 / 3 | 47 / 0 / 0 | 32 / 4 / 3 | 30 / 5 / 4 |

closure-place10m: arm trials/wins far 72/4, flip 0/0, replace 55/1, room 14/0, rotate 53/3, shift 90/9, swap 52/4, window 63/1
  1Bitsy_1bitsy: 8 trials, 1 kept, 1 parts moved, stop: no candidates, 574.5 s
    + far: J1 shift -y 1.50 mm (6 -> 4)
  96boards-robomezzi_96boards-robomezzi: 40 trials, 1 kept, 110 parts moved, stop: route budget, 1780.4 s
    + replace: new placement of the whole board (3) (2 -> 1)
  ErgodoneWithHole_ErgoDone: 10 trials, 1 kept, 1 parts moved, stop: no candidates, 1291.0 s
    + rotate: SW1:9 rotate 180 (3 -> 2)
  Hardware_Playground_Touch_Switch_2ch_PCB: 30 trials, 3 kept, 2 parts moved, stop: no candidates, 508.6 s
    + shift: X3 shift -y 1.00 mm (5 -> 4)
    + far: X3 shift -y 2.00 mm (4 -> 3)
    + shift: L2 shift -x 0.50 mm (3 -> 2)
  Hardware_Playground_sd_wav_pcm5100: 19 trials, 1 kept, 1 parts moved, stop: no candidates, 611.1 s
    + shift: C11 shift +x 1.00 mm (3 -> 2)
  Hardware_Playground_serial_gw_ATMEGA328P: 4 trials, 0 kept, 0 parts moved, stop: no candidates, 289.2 s
  bullion_bullion: 40 trials, 0 kept, 0 parts moved, stop: route budget, 519.4 s
  front-end-modules_LimeSDR_Sony: 28 trials, 3 kept, 3 parts moved, stop: time limit, 1875.5 s
    + shift: R60 shift -y 1.00 mm (8 -> 6)
    + shift: R61 shift +y 1.00 mm (6 -> 3)
    + rotate: C82 rotate 90 (3 -> 2)
  kitspace_ardfpga: 40 trials, 3 kept, 4 parts moved, stop: route budget, 1126.6 s
    + swap: R10 swap R11 (11 -> 10)
    + far: U1 shift +x 1.50 mm (10 -> 9)
    + far: RN3 shift +x 2.00 mm (9 -> 8)
  kitspace_teensy-fx: 40 trials, 4 kept, 6 parts moved, stop: route budget, 1278.6 s
    + swap: D10 swap D16 (11 -> 9)
    + swap: D16 swap D18 (9 -> 8)
    + swap: D4 swap D11 (8 -> 7)
    + shift: U3 shift +y 0.50 mm (7 -> 6)
  mechkeys_58r-mxalps: 37 trials, 2 kept, 2 parts moved, stop: time limit, 1815.6 s
    + rotate: R3 rotate 180 (11 -> 10)
    + shift: D8 shift +x 1.00 mm (10 -> 8)
  memsarray_mems_modules: 34 trials, 0 kept, 0 parts moved, stop: no candidates, 162.8 s
  motor-high-current-sensors_CurrentSensorPCB_line: 27 trials, 1 kept, 200 parts moved, stop: time limit, 1978.9 s
    + window: re-place 14 parts around the failures (seed 1) (11 -> 6)
  phone_amp_phone_amp: 30 trials, 2 kept, 2 parts moved, stop: no candidates, 795.3 s
    + shift: R12 shift -x 1.00 mm (9 -> 8)
    + shift: U3 shift +x 0.50 mm (8 -> 6)

closure-route10m: arm trials/wins far 41/1, flip 0/0, replace 32/0, room 6/1, rotate 28/3, shift 51/7, swap 19/1, window 37/3
  1Bitsy_1bitsy: 1 trials, 1 kept, 1 parts moved, stop: complete, 129.4 s
    + shift: C15 shift +y 0.50 mm (1 -> 0)
  96boards-robomezzi_96boards-robomezzi: 13 trials, 0 kept, 0 parts moved, stop: time limit, 1843.5 s
  ErgodoneWithHole_ErgoDone: 10 trials, 1 kept, 1 parts moved, stop: no candidates, 1463.5 s
    + rotate: SW1:9 rotate 180 (3 -> 2)
  Hardware_Playground_Touch_Switch_2ch_PCB: 1 trials, 1 kept, 1 parts moved, stop: complete, 154.7 s
    + shift: X3 shift -y 1.00 mm (3 -> 0)
  Hardware_Playground_sd_wav_pcm5100: 19 trials, 1 kept, 1 parts moved, stop: no candidates, 1571.8 s
    + shift: C11 shift +x 1.00 mm (3 -> 2)
  Hardware_Playground_serial_gw_ATMEGA328P: 4 trials, 0 kept, 0 parts moved, stop: no candidates, 471.6 s
  bullion_bullion: 40 trials, 1 kept, 8 parts moved, stop: route budget, 1458.3 s
    + window: re-place 8 parts around the failures (seed 1) (13 -> 12)
  front-end-modules_LimeSDR_Sony: 2 trials, 2 kept, 2 parts moved, stop: complete, 482.6 s
    + shift: C78 shift -x 0.50 mm (2 -> 1)
    + shift: C38 shift -y 0.50 mm (1 -> 0)
  kitspace_ardfpga: 20 trials, 1 kept, 1 parts moved, stop: time limit, 1803.3 s
    + shift: RN3 shift +x 1.00 mm (7 -> 5)
  kitspace_teensy-fx: 15 trials, 2 kept, 3 parts moved, stop: time limit, 1869.4 s
    + rotate: RV8 rotate 270 (5 -> 2)
    + swap: D5 swap D4 (2 -> 1)
  mechkeys_58r-mxalps: 11 trials, 3 kept, 16 parts moved, stop: complete, 1444.9 s
    + rotate: R3 rotate 180 (6 -> 5)
    + window: re-place 15 parts around the failures (seed 2) (5 -> 2)
    + shift: PROGRAM shift +y 0.50 mm (2 -> 0)
  memsarray_mems_modules: 34 trials, 0 kept, 0 parts moved, stop: no candidates, 793.2 s
  motor-high-current-sensors_CurrentSensorPCB_line: 16 trials, 3 kept, 30 parts moved, stop: time limit, 2060.7 s
    + far: D2 shift +y 4.00 mm (6 -> 4)
    + window: re-place 34 parts around the failures (seed 1) (4 -> 3)
    + room: room around a failed pad at (87.20, 131.09): C68,C69 pushed 1.00 mm (3 -> 1)
  phone_amp_phone_amp: 20 trials, 0 kept, 0 parts moved, stop: no candidates, 1462.8 s
