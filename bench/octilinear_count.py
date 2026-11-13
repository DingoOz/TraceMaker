#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Off-angle track segments of routed boards (doc 05 §80, D187): segments not along 0, 45 or 90 degrees, and short ones.

  bench/octilinear_count.py RUN_DIR [RUN_DIR ...] [--json out.json]

Counts the (segment ...) items of every board in RUN_DIR/boards/*.kicad_pcb, with the engine's test (src/passes/octilinear.cpp,
off_octilinear_segment: more than 0.1 degree from an axis and from a diagonal). A short segment is one shorter than its width.
Boards are matched by name across runs; totals are over the boards every run has. Arcs are not counted.
"""
import argparse
import glob
import json
import math
import os
import re
import sys

SEG = re.compile(r"\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)")
TAN = 0.0017453  # tan(0.1 degree), as the engine's 17'453 / 10'000'000


def off_angle(dx: float, dy: float) -> bool:
    m, big = min(abs(dx), abs(dy)), max(abs(dx), abs(dy))
    if m < big * TAN:
        return False
    return (big - m) >= (big + m) * TAN


def count(path: str) -> dict:
    n = off = short = 0
    off_len = 0.0
    for m in SEG.finditer(open(path, encoding="utf-8").read()):
        x1, y1, x2, y2, w = map(float, m.groups())
        dx, dy = x2 - x1, y2 - y1
        length = math.hypot(dx, dy)
        if length == 0:
            continue
        n += 1
        if off_angle(dx, dy):
            off += 1
            off_len += length
        if length < w:
            short += 1
    return {"segments": n, "off_angle": off, "off_angle_mm": round(off_len, 2), "short": short}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--json")
    a = ap.parse_args()
    data = {}
    for run in a.runs:
        boards = sorted(glob.glob(os.path.join(run, "boards", "*.kicad_pcb")))
        data[run] = {os.path.basename(b)[: -len(".kicad_pcb")]: count(b) for b in boards}
    common = sorted(set.intersection(*(set(d) for d in data.values())))
    print(f"{len(common)} boards in every run")
    print(f"{'run':40} {'segments':>9} {'off-angle':>10} {'off mm':>9} {'short':>7}")
    totals = {}
    for run, d in data.items():
        t = {k: sum(d[b][k] for b in common) for k in ("segments", "off_angle", "off_angle_mm", "short")}
        totals[run] = t
        print(f"{os.path.basename(run.rstrip('/')):40} {t['segments']:9d} {t['off_angle']:10d} {t['off_angle_mm']:9.1f} {t['short']:7d}")
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"boards": common, "totals": totals, "per_board": {r: {b: d[b] for b in common} for r, d in data.items()}}, f, indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
