# SPDX-License-Identifier: GPL-3.0-or-later
"""Solution drawings: the routed board with every compromise of the ledger marked by class, and a zoom on the densest cluster. Usage: figs_solution.py BOARD LAYOUT SHORT"""
import json, pathlib, sys, collections
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import draw, kgeom
import matplotlib
import matplotlib.pyplot as plt
from matplotlib.lines import Line2D

ROOT = pathlib.Path("/home/dingo/Programming/TraceMaker/reports/realworld")
board, layout, short = sys.argv[1:4]
d = ROOT / "boards" / board / layout
b = kgeom.load(str(d / "routed.kicad_pcb"))
led = json.load(open(d / "ledger.json"))
COL = {"A": "#1a9850", "B": "#f39c12", "C": "#d73027", "D": "#7b0099", "j": "#4575b4", "m": "#4575b4", "a": "#4575b4", "o": "#000000"}
marks = []
for a in led["added"]:
    if not a["pos"] or a["type"] in ("track_dangling",):
        continue
    c = a["class"][0]
    marks.append({"x": a["pos"][0], "y": a["pos"][1], "color": COL.get(c, "#777"), "r": 0.5, "cls": c, "type": a["type"]})
x0, y0, x1, y1 = draw.bounds(b)
w, h = x1 - x0, y1 - y0
fig = plt.figure(figsize=(6.4, 6.4 * min(h / w, 1.55)))
ax = fig.add_axes([0.01, 0.01, 0.98, 0.98])
draw.draw(b, ax, marks=marks, window=(x0 - 1, y0 - 1, x1 + 1, y1 + 1), mark_scale=1.0, pad_alpha=0.35)
leg = [Line2D([0], [0], marker="o", color="w", markeredgecolor=COL[k], markerfacecolor="none", markersize=8, label=l) for k, l in
       (("A", "A: conventional (gap/width at least 0.15 mm)"), ("B", "B: reduced margin (0.127 to 0.15 mm)"), ("j", "judgement item (via drill, thermal, hole)"))]
ax.legend(handles=leg, loc="lower left", fontsize=6.5, framealpha=0.9)
fig.savefig(ROOT / "figs" / f"{short}_sol_{layout}.pdf")
fig.savefig(ROOT / "figs" / f"{short}_sol_{layout}.png", dpi=170)
plt.close(fig)
# densest 10 x 10 mm cell
cnt = collections.Counter((int(m["x"] // 5), int(m["y"] // 5)) for m in marks)
(cx, cy), n = cnt.most_common(1)[0]
zx0, zy0 = cx * 5 - 2.5, cy * 5 - 2.5
fig = plt.figure(figsize=(6.4, 6.4))
ax = fig.add_axes([0.01, 0.01, 0.98, 0.98])
draw.draw(b, ax, marks=marks, window=(zx0, zy0, zx0 + 15, zy0 + 15), mark_scale=1.0, labels=tuple(p.ref for p in b.parts if zx0 < p.x < zx0 + 15 and zy0 < p.y < zy0 + 15 and len(p.pads) > 6), pad_alpha=0.35)
fig.savefig(ROOT / "figs" / f"{short}_zoom_{layout}.pdf")
fig.savefig(ROOT / "figs" / f"{short}_zoom_{layout}.png", dpi=170)
print(short, layout, "marks", len(marks), "densest cell", cx * 5, cy * 5, n)
