#!/usr/bin/env python3
"""Straight-line crossings between connections (ball -> test point) on every srj20 board: a lower bound on how much the
connections must cross each other (each crossing needs the two on different layers there, i.e. vias). Compared with the ledger."""
import json, pathlib, re, itertools, statistics
FIX = pathlib.Path("/home/dingo/Programming/TraceMaker/bench/data/tscircuit/boards/srj20")
OPEN = {"sample152", "sample040", "sample070", "sample064", "sample176", "sample120", "sample008", "sample088", "sample144", "sample200", "sample032"}
LATE = {"sample128", "sample103", "sample072", "sample096"}  # first clean 2026-11-15/16

def pads(txt):
    out = {}
    for blk in txt.split("\t(footprint ")[1:]:
        at = re.search(r"\(at ([-\d.]+) ([-\d.]+)( [-\d.]+)?\)", blk)
        ox, oy = float(at[1]), float(at[2])
        for m in re.finditer(r'\(pad "[^"]*" \w+ \w+\s+\(at ([-\d.]+) ([-\d.]+)[^)]*\)[\s\S]*?\(net "([^"]+)"\)', blk):
            out.setdefault(m[3], []).append((ox + float(m[1]), oy + float(m[2])))
    return out

def cross(p, q):
    (a, b), (c, d) = p, q
    o = lambda u, v, w: (v[0] - u[0]) * (w[1] - u[1]) - (v[1] - u[1]) * (w[0] - u[0])
    return o(a, b, c) * o(a, b, d) < 0 and o(c, d, a) * o(c, d, b) < 0

rows = []
for d in sorted(FIX.iterdir()):
    txt = (d / "unrouted.kicad_pcb").read_text()
    conns = [tuple(v[:2]) for v in pads(txt).values() if len(v) == 2]
    x = sum(cross(p, q) for p, q in itertools.combinations(conns, 2))
    L = sum(((a[0]-b[0])**2 + (a[1]-b[1])**2) ** 0.5 for a, b in conns)
    rows.append((d.name, len(conns), x, L))
grp = lambda r: "open" if r[0] in OPEN else "late" if r[0] in LATE else "clean"
for g in ("clean", "late", "open"):
    rs = [r for r in rows if grp(r) == g]
    print(g, len(rs), "conns median", statistics.median(r[1] for r in rs), "crossings median", statistics.median(r[2] for r in rs),
          "crossings/conn median", round(statistics.median(r[2] / r[1] for r in rs), 2), "length mm median", round(statistics.median(r[3] for r in rs)))
rk = sorted(rows, key=lambda r: -r[2])
print("rank by crossings of the open boards (1 = most):", sorted((rk.index(r) + 1, r[0], r[1], r[2]) for r in rows if r[0] in OPEN))
rc = sorted(rows, key=lambda r: -r[1])
print("rank by connections:", sorted((rc.index(r) + 1, r[0]) for r in rows if r[0] in OPEN))
json.dump(rows, open("crossings.json", "w"))
