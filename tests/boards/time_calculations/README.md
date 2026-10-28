# time_calculations

KiCad's own test board for propagation delays, copied unchanged from the KiCad source tree, tag `10.0.6`:
`qa/data/pcbnew/time_calculations.kicad_{pcb,pro,dru}` (KiCad is GPL-3.0-or-later; see https://gitlab.com/kicad/code/kicad).

Seven net classes, each with a tuning profile (`.kicad_pro`), and one rule per class in picoseconds (`.kicad_dru`). Classes 1 to 6
set a delay window of ±0.01 ps around the value KiCad computes; class 7 sets a skew. KiCad 10.0.6's DRC reports one violation on the
board: `skew_out_of_range` for CLASS7, because 10.0.6 measures the longest net against itself (doc 05 §59). `tests/test_length.cpp`
checks that TraceMaker's DRC reports the same and computes each class's delay.
