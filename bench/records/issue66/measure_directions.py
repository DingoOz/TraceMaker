#!/usr/bin/env python3
"""Share of track length per copper layer that runs horizontal, vertical, at 45 degrees or at another angle (issue #66).

    measure_directions.py BOARD.kicad_pcb [BOARD2.kicad_pcb ...] [--json OUT] [--tol DEG]

Every (segment ...) of the board counts (the boards measured here had their tracks removed before routing, so all
segments are the router's); arcs are counted as "other" by their chord. An angle within --tol degrees (default 1) of
0, 90 or 45/135 degrees is horizontal, vertical or 45 degrees.
"""
import argparse
import json
import math
import re
import sys

SEG = re.compile(r'\((segment|arc)\s+(.*?)\(layer\s+"([^"]+)"\)', re.S)
START = re.compile(r'\(start\s+([-\d.]+)\s+([-\d.]+)\)')
END = re.compile(r'\(end\s+([-\d.]+)\s+([-\d.]+)\)')


def measure(path, tol):
    text = open(path, encoding="utf-8").read()
    per = {}
    for kind, body, layer in SEG.findall(text):
        s, e = START.search(body), END.search(body)
        if not s or not e:
            continue
        x0, y0, x1, y1 = (float(v) for v in (*s.groups(), *e.groups()))
        dx, dy = x1 - x0, y1 - y0
        length = math.hypot(dx, dy)
        if length == 0:
            continue
        a = math.degrees(math.atan2(dy, dx)) % 180.0
        if kind == "arc":
            cls = "other"
        elif min(a, 180 - a) <= tol:
            cls = "horizontal"
        elif abs(a - 90) <= tol:
            cls = "vertical"
        elif abs(a - 45) <= tol or abs(a - 135) <= tol:
            cls = "diag45"
        else:
            cls = "other"
        d = per.setdefault(layer, {"horizontal": 0.0, "vertical": 0.0, "diag45": 0.0, "other": 0.0})
        d[cls] += length
    out = {}
    for layer in sorted(per):
        d = per[layer]
        tot = sum(d.values())
        out[layer] = {"length_mm": round(tot, 3), **{k: round(100 * v / tot, 1) for k, v in d.items()}}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--json")
    ap.add_argument("--tol", type=float, default=1.0)
    a = ap.parse_args()
    res = {}
    for b in a.boards:
        res[b] = measure(b, a.tol)
        for layer, d in res[b].items():
            print(f"{b}  {layer:8s} {d['length_mm']:9.1f} mm  h {d['horizontal']:5.1f}  v {d['vertical']:5.1f}  45 {d['diag45']:5.1f}  other {d['other']:5.1f} %")
    if a.json:
        json.dump(res, open(a.json, "w"), indent=1)


if __name__ == "__main__":
    sys.exit(main())
