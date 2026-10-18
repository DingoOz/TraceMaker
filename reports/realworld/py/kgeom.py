# SPDX-License-Identifier: GPL-3.0-or-later
"""Small KiCad .kicad_pcb reader for the real-world report: parts, pads, tracks, vias, outline, rules. Works on the 2017 (module) and the
10.x (footprint) formats; units mm, board coordinates as in the file (y down)."""
from __future__ import annotations

import math
import re
from dataclasses import dataclass, field


def tokenize(text: str):
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if c in " \t\r\n":
            i += 1
        elif c in "()":
            yield c
            i += 1
        elif c == '"':
            j = i + 1
            buf = []
            while j < n and text[j] != '"':
                if text[j] == "\\" and j + 1 < n:
                    j += 1
                buf.append(text[j])
                j += 1
            yield ("s", "".join(buf))
            i = j + 1
        else:
            j = i
            while j < n and text[j] not in " \t\r\n()":
                j += 1
            yield text[i:j]
            i = j


def parse(text: str):
    stack: list[list] = [[]]
    for tok in tokenize(text):
        if tok == "(":
            stack.append([])
        elif tok == ")":
            done = stack.pop()
            stack[-1].append(done)
        elif isinstance(tok, tuple):
            stack[-1].append(tok[1])
        else:
            stack[-1].append(tok)
    return stack[0][0]


def find(node, name):
    return [c for c in node if isinstance(c, list) and c and c[0] == name]


def first(node, name):
    r = find(node, name)
    return r[0] if r else None


def num(x) -> float:
    return float(x)


@dataclass
class Pad:
    ref: str
    num: str
    x: float
    y: float
    w: float
    h: float
    shape: str
    kind: str
    layers: list
    net: int
    netname: str
    drill: float = 0.0
    rot: float = 0.0
    rratio: float = 0.0


@dataclass
class Part:
    ref: str
    value: str
    fp: str
    x: float
    y: float
    rot: float
    side: str
    pads: list = field(default_factory=list)
    locked: bool = False


@dataclass
class Track:
    x1: float
    y1: float
    x2: float
    y2: float
    w: float
    layer: str
    net: int


@dataclass
class Via:
    x: float
    y: float
    size: float
    drill: float
    net: int


@dataclass
class Board:
    parts: list
    tracks: list
    vias: list
    nets: dict
    edge: list
    layers: list
    rules: dict
    classes: dict


def load(path: str) -> Board:
    root = parse(open(path, errors="replace").read())
    nets = {}
    for n in find(root, "net"):
        nets[int(n[1])] = n[2] if len(n) > 2 else ""
    layers = [l[1] for l in find(first(root, "layers"), "") if False]
    lay = first(root, "layers")
    layers = [c[1] for c in lay if isinstance(c, list) and len(c) > 2 and c[2] in ("signal", "power", "mixed", "jumper")] if lay else []
    parts = []
    for m in find(root, "module") + find(root, "footprint"):
        at = first(m, "at")
        px, py = num(at[1]), num(at[2])
        rot = num(at[3]) if len(at) > 3 else 0.0
        side = "B" if (first(m, "layer") or ["", "F.Cu"])[1].startswith("B") else "F"
        ref = val = ""
        for t in find(m, "fp_text"):
            if t[1] == "reference":
                ref = t[2]
            elif t[1] == "value":
                val = t[2]
        for t in find(m, "property"):
            if t[1] == "Reference":
                ref = t[2]
            elif t[1] == "Value":
                val = t[2]
        part = Part(ref, val, m[1] if isinstance(m[1], str) else "", px, py, rot, side, locked=any(c == "locked" for c in m if isinstance(c, str)))
        cr, sr = math.cos(math.radians(-rot)), math.sin(math.radians(-rot))
        for p in find(m, "pad"):
            pat = first(p, "at")
            lx, ly = num(pat[1]), num(pat[2])
            prot = num(pat[3]) if len(pat) > 3 else rot
            gx, gy = px + lx * cr - ly * sr, py + lx * sr + ly * cr
            size = first(p, "size")
            dr = first(p, "drill")
            drill = 0.0
            if dr:
                vals = [v for v in dr[1:] if isinstance(v, str) and re.fullmatch(r"[0-9.]+", v)]
                drill = float(vals[0]) if vals else 0.0
            nt = first(p, "net")
            pl = first(p, "layers")
            part.pads.append(Pad(ref, str(p[1]), gx, gy, num(size[1]), num(size[2]), p[3] if len(p) > 3 else "", p[2], pl[1:] if pl else [],
                                 int(nt[1]) if nt else 0, nt[2] if nt and len(nt) > 2 else "", drill, prot, float((first(p, "roundrect_rratio") or [0, 0])[1])))
        parts.append(part)
    tracks = []
    for s in find(root, "segment"):
        st, en, w, l, n = first(s, "start"), first(s, "end"), first(s, "width"), first(s, "layer"), first(s, "net")
        tracks.append(Track(num(st[1]), num(st[2]), num(en[1]), num(en[2]), num(w[1]), l[1], int(n[1]) if n else 0))
    vias = []
    for v in find(root, "via"):
        at, sz, dr, n = first(v, "at"), first(v, "size"), first(v, "drill"), first(v, "net")
        vias.append(Via(num(at[1]), num(at[2]), num(sz[1]), num(dr[1]) if dr else 0.0, int(n[1]) if n else 0))
    edge = []
    for tag in ("gr_line", "gr_arc", "gr_rect", "gr_poly"):
        for g in find(root, tag):
            l = first(g, "layer")
            if l and l[1] == "Edge.Cuts":
                st, en = first(g, "start"), first(g, "end")
                if st and en:
                    edge.append((num(st[1]), num(st[2]), num(en[1]), num(en[2]), tag))
    rules, classes = {}, {}
    setup = first(root, "setup")
    if setup:
        for c in setup:
            if isinstance(c, list) and len(c) >= 2 and isinstance(c[1], str) and re.fullmatch(r"-?[0-9.]+", c[1]):
                rules[c[0]] = float(c[1])
    for nc in find(root, "net_class"):
        d = {c[0]: float(c[1]) for c in nc if isinstance(c, list) and len(c) == 2 and isinstance(c[1], str) and re.fullmatch(r"-?[0-9.]+", c[1])}
        d["nets"] = [c[1] for c in find(nc, "add_net")]
        classes[nc[1]] = d
    return Board(parts, tracks, vias, nets, edge, layers, rules, classes)
