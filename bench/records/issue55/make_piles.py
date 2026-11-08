#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Issue #55 public boards: PCBench boards with a large decoupled IC, every part full mode may move piled at the board
centre (the way bench/make_place_sets.py builds set S). Writes bench/data/issue55-S/<board>/unrouted.kicad_pcb.

  bench/records/issue55/make_piles.py Board1 Board2 ..."""
import importlib.util
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]
spec = importlib.util.spec_from_file_location("make_place_sets", ROOT / "bench/make_place_sets.py")
mps = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mps)
mps.SDIR = ROOT / "bench/data/issue55-S"
for name in sys.argv[1:]:
    print(mps.scratch_board(name))
