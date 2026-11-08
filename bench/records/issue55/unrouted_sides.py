#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Issue #55: the route log's `unrouted:` lines that end on a pin of the named ICs, counted by the side of the IC
the pin is on (the board the route started from gives the positions).

  unrouted_sides.py placed.kicad_pcb route.log U10 U11"""
import re
import sys
from collections import Counter

import decap_sides as ds

board, log, ics = sys.argv[1], sys.argv[2], sys.argv[3:]
b = ds.load(board)
parts = {p.ref: p for p in b.parts}
total = 0
per = {ic: Counter() for ic in ics}
for line in open(log, errors="replace"):
    m = re.match(r"\s+unrouted: .*?: (\S+) -> (\S+)", line)
    if not m:
        continue
    total += 1
    for end in m.groups():
        ref, _, num = end.partition(".")
        if ref in per and num:
            u = parts[ref]
            xs = [q.x for q in u.pads]
            ys = [q.y for q in u.pads]
            cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
            hw, hh = (max(xs) - min(xs)) / 2 or 1, (max(ys) - min(ys)) / 2 or 1
            q = next((q for q in u.pads if q.num == num), None)
            if q is None:
                continue
            dx, dy = (q.x - cx) / hw, (q.y - cy) / hh
            s = ("right" if dx > 0 else "left") if abs(dx) >= abs(dy) else ("bottom" if dy > 0 else "top")
            per[ref][s] += 1
print(f"unrouted connections: {total}")
for ic, c in per.items():
    print(f"{ic}: pin ends of unrouted connections L {c['left']} R {c['right']} T {c['top']} B {c['bottom']} (total {sum(c.values())})")
