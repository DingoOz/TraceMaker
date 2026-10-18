#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""crop_plot.py BOARD.kicad_pcb OUT.png x0 y0 x1 y1 [--mark REF.PIN ...]: copper of a region as a picture (F.Cu red, B.Cu blue, vias black, pads grey)."""
import sys, math
sys.path.insert(0, "/home/dingo/Programming/TraceMaker/reports/realworld/py")
import kgeom
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
from matplotlib.patches import Rectangle, Circle

pcb, out = sys.argv[1], sys.argv[2]
x0, y0, x1, y1 = map(float, sys.argv[3:7])
marks = set(sys.argv[sys.argv.index("--mark") + 1:]) if "--mark" in sys.argv else set()
b = kgeom.load(pcb)
fig, ax = plt.subplots(figsize=(10, 10 * (y1 - y0) / (x1 - x0)), dpi=110)
for part in b.parts:
    for p in part.pads:
        if x0 - 3 < p.x < x1 + 3 and y0 - 3 < p.y < y1 + 3:
            w, h = (p.w, p.h) if abs(math.sin(math.radians(p.rot))) < 0.7 else (p.h, p.w)
            col = "#bbbbbb" if "F.Cu" in p.layers or "*.Cu" in p.layers else "#9ab"
            ax.add_patch(Rectangle((p.x - w / 2, p.y - h / 2), w, h, fc=col, ec="#666", lw=0.4, zorder=2))
            lab = f"{part.ref}.{p.num}"
            ax.text(p.x, p.y, p.num, fontsize=4, ha="center", va="center", zorder=6, color="#222")
            if lab in marks:
                ax.add_patch(Circle((p.x, p.y), 0.5, fc="none", ec="#e0a000", lw=1.2, zorder=7))
for t in b.tracks:
    if max(t.x1, t.x2) < x0 or min(t.x1, t.x2) > x1 or max(t.y1, t.y2) < y0 or min(t.y1, t.y2) > y1:
        continue
    ax.plot([t.x1, t.x2], [t.y1, t.y2], color="#d33" if t.layer == "F.Cu" else "#35c", lw=max(0.5, t.w * 72 / 25.4 * 10 * 0.0 + t.w * 13), solid_capstyle="round", alpha=0.8, zorder=3 if t.layer == "F.Cu" else 2)
for v in b.vias:
    ax.add_patch(Circle((v.x, v.y), v.size / 2, fc="#222", ec="none", zorder=5))
ax.set_xlim(x0, x1); ax.set_ylim(y1, y0); ax.set_aspect("equal")
ax.tick_params(labelsize=6)
fig.savefig(out, bbox_inches="tight")
