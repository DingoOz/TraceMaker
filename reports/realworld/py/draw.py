# SPDX-License-Identifier: GPL-3.0-or-later
"""Board drawings for the report (matplotlib; run inside the toolbox image)."""
from __future__ import annotations

import json
import math
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import matplotlib

matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Polygon, Circle

import geomx
import kgeom

LAYER_COL = {"F.Cu": "#c83232", "In1.Cu": "#2e9e44", "In2.Cu": "#c9a227", "B.Cu": "#2f5fc4"}


def bounds(b: kgeom.Board):
    xs, ys = [], []
    for e in b.edge:
        xs += [e[0], e[2]]
        ys += [e[1], e[3]]
    if not xs:
        for p in b.parts:
            xs.append(p.x)
            ys.append(p.y)
    return min(xs), min(ys), max(xs), max(ys)


def draw(b: kgeom.Board, ax, tracks=True, pads=True, layers=None, labels=(), unconnected=(), marks=(), window=None, mark_scale=1.0, pad_alpha=0.55, label_size=6):
    x0, y0, x1, y1 = window or bounds(b)
    ax.set_xlim(x0, x1)
    ax.set_ylim(y1, y0)
    ax.set_aspect("equal")
    ax.set_xticks([])
    ax.set_yticks([])
    for e in b.edge:
        ax.plot([e[0], e[2]], [e[1], e[3]], color="#333", lw=0.8, zorder=1)
    fig = ax.figure
    pt_per_mm = (ax.get_position().width * fig.get_figwidth() * 72) / (x1 - x0)
    if pads:
        for part in b.parts:
            for p in part.pads:
                col = "#c83232" if any(l.startswith("F.") or l == "*.Cu" for l in p.layers) else "#2f5fc4"
                if p.kind == "thru_hole":
                    col = "#9a6b00"
                if p.shape in ("circle",):
                    ax.add_patch(Circle((p.x, p.y), min(p.w, p.h) / 2, fc=col, ec="none", alpha=pad_alpha, zorder=2))
                else:
                    ax.add_patch(Polygon(geomx.rect_corners(p.x, p.y, p.w, p.h, p.rot), closed=True, fc=col, ec="none", alpha=pad_alpha, zorder=2))
    if tracks:
        for t in b.tracks:
            if layers and t.layer not in layers:
                continue
            ax.plot([t.x1, t.x2], [t.y1, t.y2], color=LAYER_COL.get(t.layer, "#888"), lw=max(0.4, t.w * pt_per_mm), solid_capstyle="round", alpha=0.85, zorder=3)
        for v in b.vias:
            ax.add_patch(Circle((v.x, v.y), v.size / 2, fc="#bbbbbb", ec="#444", lw=0.3, zorder=4))
            ax.add_patch(Circle((v.x, v.y), v.drill / 2, fc="white", ec="none", zorder=5))
    for part in b.parts:
        if part.ref in labels:
            ax.text(part.x, part.y, part.ref, fontsize=label_size, ha="center", va="center", color="black", zorder=8,
                    bbox=dict(boxstyle="round,pad=0.1", fc="white", ec="none", alpha=0.7))
    for (xa, ya, xb, yb) in unconnected:
        ax.plot([xa, xb], [ya, yb], color="black", lw=0.9, ls=(0, (2, 1.5)), zorder=7)
        for (x, y) in ((xa, ya), (xb, yb)):
            ax.add_patch(Circle((x, y), 0.45 * mark_scale, fc="none", ec="black", lw=0.8, zorder=7))
    for m in marks:
        ax.add_patch(Circle((m["x"], m["y"]), m.get("r", 0.6) * mark_scale, fc="none", ec=m.get("color", "#ff00ff"), lw=1.1, zorder=9))


def unconnected_from_drc(path: str):
    j = json.loads(pathlib.Path(path).read_text())
    out = []
    for u in j.get("unconnected_items", []):
        it = u["items"]
        if len(it) >= 2:
            out.append((it[0]["pos"]["x"], it[0]["pos"]["y"], it[1]["pos"]["x"], it[1]["pos"]["y"]))
    return out
