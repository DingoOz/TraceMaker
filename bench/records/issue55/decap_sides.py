#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Decoupling-capacitor distribution round an IC (issue #55): for each IC named, the two-pad C* parts between one of
its supply nets (power-like names) and ground whose nearest IC on that supply is this one, counted by the side of the IC they sit on (the IC's diagonals split the plane into four
sectors, in the IC's own frame), by board side, and their distance to the nearest IC pad on their supply.

Usage: decap_sides.py board.kicad_pcb U10 [U11 ...] [--json]"""
import json
import re
import math
import os
import sys

from types import SimpleNamespace as NS


def tokens(text):
    for m in re.finditer(r'\(|\)|"(?:[^"\\]|\\.)*"|[^\s()]+', text):
        t = m.group(0)
        yield ("s", t[1:-1].replace('\\"', '"')) if t[0] == '"' else t


def parse(text):
    st = [[]]
    for t in tokens(text):
        if t == "(":
            st.append([])
        elif t == ")":
            d = st.pop()
            st[-1].append(d)
        else:
            st[-1].append(t[1] if isinstance(t, tuple) else t)
    return st[0][0]


def kids(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def load(path):
    """Footprints (ref, position, side) with absolute pad positions and net names; KiCad 5-10 formats."""
    root = parse(open(path, errors="replace").read())
    netnames = {n[1]: n[2] for n in kids(root, "net") if len(n) > 2}
    parts = []
    for fp in kids(root, "footprint") + kids(root, "module"):
        at = kids(fp, "at")[0]
        x, y, rot = float(at[1]), float(at[2]), float(at[3]) if len(at) > 3 else 0.0
        layer = kids(fp, "layer")[0][1]
        ref = ""
        for pr in kids(fp, "property"):
            if pr[1] == "Reference":
                ref = pr[2]
        for t in kids(fp, "fp_text"):
            if t[1] == "reference":
                ref = t[2]
        c, s = math.cos(math.radians(rot)), math.sin(math.radians(rot))
        pads = []
        for p in kids(fp, "pad"):
            pa = kids(p, "at")[0]
            px, py = float(pa[1]), float(pa[2])
            nt = kids(p, "net")
            name = ""
            if nt:
                name = nt[0][2] if len(nt[0]) > 2 else netnames.get(nt[0][1], nt[0][1])
            pads.append(NS(num=p[1], x=x + px * c + py * s, y=y - px * s + py * c, net=name or None, netname=name))
        parts.append(NS(ref=ref, x=x, y=y, side=layer, pads=pads))
    return NS(parts=parts, nets={})

GND = ("GND", "VSS", "AGND", "DGND", "PGND")


def is_supply(name):
    """Power-net names (as the placer's power_like_name, roughly): +3V3, VCC, VDD_x, 1V8, ..."""
    leaf = name.split("/")[-1].upper()
    return bool(re.search(r"^\+|VCC|VDD|VBAT|VIN|^\d+V\d*|^V\d", leaf)) and not is_gnd(name)


def is_gnd(name):
    leaf = name.split("/")[-1].upper()
    return leaf.startswith(GND) or leaf.endswith("GND")


def analyse(path, ics):
    b = load(path)
    parts = {p.ref: p for p in b.parts}
    out = {}
    for ic in ics:
        u = parts[ic]
        sup = {}
        for q in u.pads:
            if q.net and is_supply(q.netname):
                sup.setdefault(q.net, []).append(q)
        xs = [q.x for q in u.pads]
        ys = [q.y for q in u.pads]
        cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
        hw, hh = (max(xs) - min(xs)) / 2 or 1, (max(ys) - min(ys)) / 2 or 1
        sides = {"left": 0, "right": 0, "top": 0, "bottom": 0}

        def side_of(x, y):
            dx, dy = (x - cx) / hw, (y - cy) / hh
            return ("right" if dx > 0 else "left") if abs(dx) >= abs(dy) else ("bottom" if dy > 0 else "top")
        pin_sides = {"left": 0, "right": 0, "top": 0, "bottom": 0}
        for qs in sup.values():
            for q in qs:
                pin_sides[side_of(q.x, q.y)] += 1
        board_side = {"F": 0, "B": 0}
        dists = []
        caps = []
        for p in b.parts:
            if not p.ref.startswith("C") or len(p.pads) != 2:
                continue
            a, c = p.pads
            n = a.net if a.net in sup and is_gnd(c.netname) else c.net if c.net in sup and is_gnd(a.netname) else None
            if n is None:
                continue
            # only caps whose nearest IC on that supply is this one
            sp = a if a.net == n else c
            best_ic = min(((math.hypot(q.x - sp.x, q.y - sp.y), r.ref) for r in b.parts if r.ref.startswith("U")
                           for q in r.pads if q.net == n), default=(0, ic))[1]
            if best_ic != ic:
                continue
            s = side_of(p.x, p.y)
            sides[s] += 1
            board_side["B" if p.side.startswith("B") else "F"] += 1
            d = min(math.hypot(q.x - sp.x, q.y - sp.y) for q in sup[n])
            dists.append(d)
            caps.append({"ref": p.ref, "net": n, "side": s, "layer": p.side, "dist_mm": round(d, 2)})
        dists.sort()
        out[ic] = {"caps": len(dists), "sides": sides, "supply_pins": pin_sides, "board_side": board_side,
                   "median_mm": round(dists[len(dists) // 2], 2) if dists else None,
                   "max_mm": round(dists[-1], 2) if dists else None, "list": caps}
    return out


if __name__ == "__main__":
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    r = analyse(args[0], args[1:])
    if "--json" in sys.argv:
        print(json.dumps(r, indent=1))
    else:
        for ic, v in r.items():
            s = v["sides"]
            print(f"{ic}: {v['caps']} caps  L {s['left']}  R {s['right']}  T {s['top']}  B {s['bottom']}  "
                  f"(supply pins L {v['supply_pins']['left']} R {v['supply_pins']['right']} T {v['supply_pins']['top']} B {v['supply_pins']['bottom']})  "
                  f"top-side {v['board_side']['F']} bottom-side {v['board_side']['B']}  median {v['median_mm']} mm  max {v['max_mm']} mm")
