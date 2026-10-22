# SPDX-License-Identifier: GPL-3.0-or-later
"""Parasitic extraction of routed copper into SPICE (ngspice) elements.

Per net a graph is built from the copper: nodes are the points where tracks, vias and pads meet (merged within 20 um); every track is a chain of
sections of at most `SECTION` mm, each a series R (rho L / w t) and L and a shunt C at its ends, the per-unit-length L' and C' coming from the
2-D field solver for the track's width over its reference plane (fd2d.py). Vias are R and L (barrel plating 25 um). Two nets that run side by side
(gap under the survey limit) are coupled section against section: mutual capacitance Cm' and coupling factor k from the field solver for the two
widths and the gap, over the overlapped length.

Assumptions (also in the report): 1 oz outer copper (35 um), 0.5 oz inner (17.5 um), FR4 er 4.5, no solder mask in the field solution,
reference planes continuous (the return-path audit says where they are not), copper at 20 C, GND ideal.
"""
from __future__ import annotations

import math
import pathlib
import sys
from dataclasses import dataclass, field

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import fd2d
import geomx
import kgeom

RHO = 1.72e-8      # ohm m, copper at 20 C
T_OUT = 35e-6
T_IN = 17.5e-6
ER = 4.5
SECTION = 2.0      # mm, longest section of a ladder
PLATING = 25e-6    # via barrel
STACKUPS = {
    # layers -> {copper layer: (height to its reference plane in mm, thickness mm)}
    4: {"F.Cu": (0.2104, 0.035), "In1.Cu": (0.2104, 0.0175), "In2.Cu": (0.2104, 0.0175), "B.Cu": (0.2104, 0.035)},
    2: {"F.Cu": (1.6, 0.035), "B.Cu": (1.6, 0.035)},
}
BOARD_T = 1.6      # mm


def _q(x, step=0.005):
    return round(round(x / step) * step, 4)


_single_cache: dict = {}
_pair_cache: dict = {}


def single_line(w: float, h: float, t: float):
    """R', L', C' per metre of one trace of width w mm over a plane h mm below (field solver)."""
    key = (_q(w), _q(h, 0.01), _q(t, 0.0025))
    if key not in _single_cache:
        w, h, t = key
        ytop = h + t + max(2.5, 3 * h)
        cond = [(-w / 2, w / 2, h, h + t)]
        C = fd2d.cap_matrix(cond, [(0.0, h, ER)], [(0.0, 0.01)], dx=0.01, ytop=ytop)[0, 0]
        L = fd2d.inductance_matrix(cond, [(0.0, h, ER)], [(0.0, 0.01)], dx=0.01, ytop=ytop)[0, 0]
        _single_cache[key] = (RHO / (w * 1e-3 * t * 1e-3), L, C)
    return _single_cache[key]


def pair_line(w1: float, w2: float, gap: float, h: float, t: float):
    """Mutual capacitance Cm' (F/m) and coupling factor k between two parallel traces with edge gap `gap` mm."""
    key = (_q(w1), _q(w2), _q(gap, 0.0025), _q(h, 0.01), _q(t, 0.0025))
    if key not in _pair_cache:
        a, b, g, hh, tt = key
        g = max(g, 0.02)
        ytop = hh + tt + max(2.5, 3 * hh)
        cond = [(-a - g / 2, -g / 2, hh, hh + tt), (g / 2, g / 2 + b, hh, hh + tt)]
        C = fd2d.cap_matrix(cond, [(0.0, hh, ER)], [(0.0, 0.01)], dx=0.01, ytop=ytop)
        Lm = fd2d.inductance_matrix(cond, [(0.0, hh, ER)], [(0.0, 0.01)], dx=0.01, ytop=ytop)
        _pair_cache[key] = (-C[0, 1], Lm[0, 1] / math.sqrt(Lm[0, 0] * Lm[1, 1]))
    return _pair_cache[key]


def via_params(drill: float, size: float, h: float = BOARD_T):
    """R (ohm) and L (H) of a plated through via, C (F) of its pad pair to the planes (rough)."""
    tp = PLATING * 1e3  # mm
    area = math.pi * (drill + tp) * tp  # mm^2
    r = RHO * (h * 1e-3) / (area * 1e-6)
    l = 0.2 * h * (math.log(4 * h / drill) + 1) * 1e-9
    return r, l


@dataclass
class Node:
    key: tuple
    c: float = 0.0


@dataclass
class Netlist:
    lines: list = field(default_factory=list)
    nodes: dict = field(default_factory=dict)      # (net, ident) -> spice name
    cap: dict = field(default_factory=dict)        # spice node -> shunt capacitance
    sections: list = field(default_factory=list)   # (net, layer, x0,y0,x1,y1, node_a, node_b, name_L, length, w)
    pin_nodes: dict = field(default_factory=dict)  # (ref, pad, net) -> spice node
    stats: dict = field(default_factory=dict)
    count: int = 0

    def name(self, prefix):
        self.count += 1
        return f"{prefix}{self.count}"


def _cluster(points, tol=0.02):
    """Union-find of points within tol; returns list of cluster ids per point."""
    cell = tol * 2
    grid: dict = {}
    parent = list(range(len(points)))

    def find(i):
        while parent[i] != i:
            parent[i] = parent[parent[i]]
            i = parent[i]
        return i

    for i, (x, y, *_r) in enumerate(points):
        cx, cy = int(x // cell), int(y // cell)
        for dx in (-1, 0, 1):
            for dy in (-1, 0, 1):
                for j in grid.get((cx + dx, cy + dy), []):
                    if math.hypot(points[j][0] - x, points[j][1] - y) <= tol:
                        parent[find(i)] = find(j)
        grid.setdefault((cx, cy), []).append(i)
    return [find(i) for i in range(len(points))]


def build_net(board: kgeom.Board, netid: int, nl: Netlist, layers: int, ground_nets=()):
    """Add the copper of one net to the netlist. Returns nothing; pin nodes are in nl.pin_nodes."""
    name = board.nets.get(netid, f"n{netid}")
    stack = STACKUPS[4 if layers >= 4 else 2]
    tracks = [t for t in board.tracks if t.net == netid]
    vias = [v for v in board.vias if v.net == netid]
    pads = [(part, p) for part in board.parts for p in part.pads if p.net == netid]
    # a track that runs through a pad of its net (neither end inside it) taps it in the middle: split the track at the pad
    split = []
    for t in tracks:
        cuts = []
        for (part, pd) in pads:
            if pd.layers and not any(l == t.layer or l == "*.Cu" for l in pd.layers):
                continue
            if abs(pd.x - (t.x1 + t.x2) / 2) > math.hypot(t.x2 - t.x1, t.y2 - t.y1) / 2 + 3:
                continue
            seg = (t.x1, t.y1, t.x2, t.y2)
            if geomx.seg_pad_gap(seg, pd) <= 0.005 and geomx.seg_pad_gap((t.x1, t.y1, t.x1, t.y1), pd) > 0.005 and geomx.seg_pad_gap((t.x2, t.y2, t.x2, t.y2), pd) > 0.005:
                _, tt = geomx.seg_point(t.x1, t.y1, t.x2, t.y2, pd.x, pd.y)
                cuts.append(min(max(tt, 0.02), 0.98))
        if not cuts:
            split.append(t)
            continue
        pts_t = [0.0] + sorted(cuts) + [1.0]
        for a, b2 in zip(pts_t, pts_t[1:]):
            split.append(kgeom.Track(t.x1 + (t.x2 - t.x1) * a, t.y1 + (t.y2 - t.y1) * a, t.x1 + (t.x2 - t.x1) * b2, t.y1 + (t.y2 - t.y1) * b2, t.w, t.layer, t.net))
    tracks = split
    # points: (x, y, kind, idx, layer)
    pts = []
    for i, t in enumerate(tracks):
        pts.append((t.x1, t.y1, "t1", i, t.layer))
        pts.append((t.x2, t.y2, "t2", i, t.layer))
    for i, v in enumerate(vias):
        pts.append((v.x, v.y, "via", i, "*"))
    for i, (part, p) in enumerate(pads):
        pts.append((p.x, p.y, "pad", i, "*" if p.kind in ("thru_hole",) else ("B.Cu" if part.side == "B" else "F.Cu")))
    cl = _cluster(pts)
    # a track that ends anywhere inside a pad of the net (not only at its centre) is connected to it: merge the clusters
    par = {c: c for c in set(cl)}

    def fnd(c):
        while par[c] != c:
            par[c] = par[par[c]]
            c = par[c]
        return c

    for i, (x, y, kind, idx, layer) in enumerate(pts):
        if kind not in ("t1", "t2"):
            continue
        for j, (px, py, pk, pidx, player) in enumerate(pts):
            if pk != "pad" or abs(px - x) > 8 or abs(py - y) > 8:
                continue
            part, pd = pads[pidx]
            if player != "*" and player != layer:
                continue
            if geomx.seg_pad_gap((x, y, x, y), pd) <= 0.005:
                par[fnd(cl[i])] = fnd(cl[j])
    # pads of one part with the same number (a tab and its pin) are one piece of copper
    first = {}
    for i, (x, y, kind, idx, layer) in enumerate(pts):
        if kind == "pad":
            part, pd = pads[idx]
            k = (part.ref, pd.num)
            if k in first:
                par[fnd(cl[i])] = fnd(cl[first[k]])
            else:
                first[k] = i
    cl = [fnd(c) for c in cl]
    # node per (cluster, layer); through elements (via, PTH pad) connect layers of the cluster
    def node(c, layer):
        k = (netid, c, layer)
        if k not in nl.nodes:
            nl.nodes[k] = f"n{netid}_{len(nl.nodes)}"
        return nl.nodes[k]

    layer_of = {}
    for (x, y, kind, idx, layer), c in zip(pts, cl):
        layer_of.setdefault(c, set())
        if layer != "*":
            layer_of[c].add(layer)
    # sections for tracks
    for i, t in enumerate(tracks):
        c1 = cl[pts.index((t.x1, t.y1, "t1", i, t.layer))]
        c2 = cl[pts.index((t.x2, t.y2, "t2", i, t.layer))]
        L = math.hypot(t.x2 - t.x1, t.y2 - t.y1)
        h, th = stack.get(t.layer, (0.2104, 0.035))
        r_pm, l_pm, c_pm = single_line(t.w, h, th)
        if L < 1e-4:
            continue
        n = max(1, math.ceil(L / SECTION))
        a = node(c1, t.layer)
        for s in range(n):
            ls = L / n
            b = node(c2, t.layer) if s == n - 1 else f"s{netid}_{nl.count + 1}"
            rn, ln = nl.name("R"), nl.name("L")
            mid = f"m{netid}_{nl.count}"
            r_s, l_s, c_s = r_pm * ls * 1e-3, l_pm * ls * 1e-3, c_pm * ls * 1e-3
            nl.lines.append(f"{rn} {a} {mid} {r_s:.6g}")
            nl.lines.append(f"{ln} {mid} {b} {l_s:.6g}")
            nl.cap[a] = nl.cap.get(a, 0.0) + c_s / 2
            nl.cap[b] = nl.cap.get(b, 0.0) + c_s / 2
            fx = (s + 0.5) / n
            nl.sections.append({"net": netid, "layer": t.layer, "x0": t.x1 + (t.x2 - t.x1) * s / n, "y0": t.y1 + (t.y2 - t.y1) * s / n,
                                "x1": t.x1 + (t.x2 - t.x1) * (s + 1) / n, "y1": t.y1 + (t.y2 - t.y1) * (s + 1) / n, "a": a, "b": b,
                                "L": ln, "R": rn, "len": ls, "w": t.w, "h": h, "t": th, "Lh": l_s})
            a = b
    # vias and PTH pads: link the layers present in their cluster (R and L of the barrel between consecutive layers)
    done = set()
    for (x, y, kind, idx, layer), c in zip(pts, cl):
        if (kind == "via" or (kind == "pad" and layer == "*")) and c not in done:
            done.add(c)
            v = vias[idx] if kind == "via" else None
            drill = v.drill if v else (pads[idx][1].drill or 0.8)
            r, l = via_params(drill, v.size if v else 1.4)
            lyrs = sorted({k[2] for k in nl.nodes if k[0] == netid and k[1] == c}) or ["F.Cu"]
            if kind == "pad":
                lyrs = sorted(set(lyrs) | {"F.Cu", "B.Cu"})
            for lo, hi in zip(lyrs, lyrs[1:]):
                mid = nl.name("v")
                nl.lines.append(f"{nl.name('R')} {node(c, lo)} {mid} {r:.6g}")
                nl.lines.append(f"{nl.name('L')} {mid} {node(c, hi)} {l:.6g}")
    for (x, y, kind, idx, layer), c in zip(pts, cl):
        if kind == "pad":
            part, p = pads[idx]
            have = sorted({k[2] for k in nl.nodes if k[0] == netid and k[1] == c})
            lyr = layer if layer != "*" else (have[0] if have else "F.Cu")
            nl.pin_nodes[(part.ref, p.num, name)] = node(c, lyr)
    nl.stats[name] = {"tracks": len(tracks), "vias": len(vias), "length": sum(math.hypot(t.x2 - t.x1, t.y2 - t.y1) for t in tracks)}


def couple(board: kgeom.Board, nl: Netlist, nets: set, max_gap: float = 0.6, gap_floor: float = 0.0):
    """Mutual capacitance and inductance between sections of different nets that run side by side."""
    secs = [s for s in nl.sections if s["net"] in nets]
    cell = 4.0
    grid: dict = {}
    for i, s in enumerate(secs):
        for gx in range(int(min(s["x0"], s["x1"]) // cell) - 1, int(max(s["x0"], s["x1"]) // cell) + 2):
            for gy in range(int(min(s["y0"], s["y1"]) // cell) - 1, int(max(s["y0"], s["y1"]) // cell) + 2):
                grid.setdefault((gx, gy, s["layer"]), []).append(i)
    done = set()
    kcount = 0
    pairs = {}
    for i, a in enumerate(secs):
        cand = set()
        for gx in range(int(min(a["x0"], a["x1"]) // cell) - 1, int(max(a["x0"], a["x1"]) // cell) + 2):
            for gy in range(int(min(a["y0"], a["y1"]) // cell) - 1, int(max(a["y0"], a["y1"]) // cell) + 2):
                cand.update(grid.get((gx, gy, a["layer"]), []))
        for j in cand:
            if j <= i:
                continue
            b = secs[j]
            if a["net"] == b["net"]:
                continue
            sa, sb = (a["x0"], a["y0"], a["x1"], a["y1"]), (b["x0"], b["y0"], b["x1"], b["y1"])
            run, cd = geomx.parallel_run(sa, sb, max_gap + (a["w"] + b["w"]) / 2)
            if run < 0.05 or cd is None:
                continue
            gap = cd - (a["w"] + b["w"]) / 2
            if gap > max_gap:
                continue
            gap = max(gap, 0.02, gap_floor)  # gap_floor: what the coupling would be at that spacing (the no-compromise reference)
            cm_pm, k = pair_line(a["w"], b["w"], gap, a["h"], a["t"])
            cm = cm_pm * run * 1e-3
            kcount += 1
            nl.lines.append(f"Cc{kcount} {a['a']} {b['a']} {cm / 2:.6g}")
            nl.lines.append(f"Cd{kcount} {a['b']} {b['b']} {cm / 2:.6g}")
            nl.lines.append(f"K{kcount} {a['L']} {b['L']} {min(k * min(run / a['len'], run / b['len'], 1.0), 0.95):.5g}")
            key = tuple(sorted((a["net"], b["net"])))
            d = pairs.setdefault(key, {"run": 0.0, "cm": 0.0, "gap_min": 9.0})
            d["run"] += run
            d["cm"] += cm
            d["gap_min"] = min(d["gap_min"], gap)
    return pairs


def emit_caps(nl: Netlist):
    for k, (node, c) in enumerate(sorted(nl.cap.items())):
        nl.lines.append(f"Cg{k} {node} 0 {c:.6g}")
