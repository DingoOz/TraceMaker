# Escape proof records (doc 20 §2.4, doc 05 §69, D161)

Branch `complete-routing` (on main `d927bfc`), 2026-10-30. Binary: `tracemaker walled` from this branch.

- `walled_sweep.py`, `legal-sweep.tsv`: the false-alarm sweep. Each board a benchmark run left KiCad-clean (tiers A-D, srj19, srj20 in
  `bench/results/full-*`; 405 boards) has its input checked. Every pad of a net with more than one pad: 41,810 pins, **0 walled in,
  0 soft-only**, 780 s CPU in all. A first version that judged pins at their class's track width raised 6 alarms on wavegen (+3V3 at
  1.02 mm; a 0.85 mm neck-down gets out), now reported as "class only".
- `open_pins.py`, `open-pins-rescue-bd.tsv`: the end pins of the connections the D160 rescue run (`bench/results/rescue-bd`) left open on
  the 33 failing PCBench tier B-D boards:

| | Count |
|---|--:|
| open connections | 727 |
| end pins checked | 1,341 |
| walled in by the placement (proof) | 0 |
| walled in only for the class's track or via | 8 (OtterPill 1, DronPi 1, sizif 5, Teensy 1) |
| walled in only by zone fills or net-less copper graphics | 14 (all E3) |
| open through congestion | 1,319 |

- `e3-hard.log`, `e3-soft.log`: E3 (`--work 20000000 --variants 1 --no-rescue`) without and with `--soft-copper-graphics`. Without it,
  303/341 routed, and 12 of the open connections end at the pins the check names. With it, 305/328: the pours make 13 connections, and
  none of those pins is open.
