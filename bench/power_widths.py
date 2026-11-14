#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Power track widths of a board (doc 05 §79, D184): length of supply-net tracks by width, and the share at or above a target.

  bench/power_widths.py BOARD.kicad_pcb [...] [--target 0.5] [--json out.json]

Supply nets are those `tracemaker route --power-width` widens (src/app/route_job.cpp, supply_net_name). Reads the board's
(segment ...) items; arcs are not counted.
"""
import argparse
import json
import math
import re
import sys

WORD = re.compile(r"^(A|D|P)?(GND|VCC|VDD|VSS|VEE|VBUS|VIN|VBAT|VBATT|VSYS|VMOT|VPP|PWR)(A|D|IO|Q)?([-_.+0-9].*)?$", re.I)
VOLTS = re.compile(r"^[+-]?[0-9]+([.,][0-9]+)?V([0-9]+)?(_.*)?$", re.I)


def supply(name: str) -> bool:
    """The test of route --power-width (src/app/route_job.cpp, supply_net_name)."""
    leaf = name.rsplit("/", 1)[-1]
    return bool(leaf) and (leaf[0] == "+" or bool(WORD.match(leaf)) or bool(VOLTS.match(leaf)))


def measure(path: str, target: float) -> dict:
    text = open(path, encoding="utf-8").read()
    names = {}
    for m in re.finditer(r'\(net (\d+) "?((?:[^"\\)\s]|\\.)*)"?\)', text):
        names.setdefault(m.group(1), m.group(2))
    by_width, total, wide, nets = {}, 0.0, 0.0, set()
    seg = re.compile(r"\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)(.*?)\(net (\d+)\)", re.S)
    for m in seg.finditer(text):
        net = names.get(m.group(7), "")
        if not supply(net):
            continue
        x1, y1, x2, y2, w = map(float, m.group(1, 2, 3, 4, 5))
        length = math.hypot(x2 - x1, y2 - y1)
        total += length
        nets.add(net)
        by_width[w] = by_width.get(w, 0.0) + length
        if w >= target - 1e-6:
            wide += length
    widths = sorted(by_width.items())
    median = None
    acc = 0.0
    for w, l in widths:
        acc += l
        if acc >= total / 2:
            median = w
            break
    return {"board": path, "supply_nets": len(nets), "supply_length_mm": round(total, 2), "at_target_mm": round(wide, 2),
            "at_target_share": round(wide / total, 4) if total else None, "median_width_mm": median,
            "by_width_mm": {f"{w:g}": round(l, 2) for w, l in widths}}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--target", type=float, default=0.5)
    ap.add_argument("--json")
    a = ap.parse_args()
    rows = [measure(b, a.target) for b in a.boards]
    for r in rows:
        print(f"{r['board']}: {r['supply_nets']} supply nets, {r['supply_length_mm']} mm, at >= {a.target} mm: {r['at_target_mm']} mm "
              f"({r['at_target_share']}), median {r['median_width_mm']} mm")
    if a.json:
        json.dump(rows, open(a.json, "w"), indent=1)
    return 0


if __name__ == "__main__":
    sys.exit(main())
