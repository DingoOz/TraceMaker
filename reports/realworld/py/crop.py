# SPDX-License-Identifier: GPL-3.0-or-later
"""crop.py BOARD.kicad_pcb X Y HALF_MM OUT.png [REF...]: both copper layers around a point (a look at one spot of a routed board)."""
import sys
sys.path.insert(0, __import__("pathlib").Path(__file__).parent.as_posix())
import kgeom, draw, matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt
path, x, y, half, out = sys.argv[1], float(sys.argv[2]), float(sys.argv[3]), float(sys.argv[4]), sys.argv[5]
b = kgeom.load(path)
fig, axs = plt.subplots(1, 2, figsize=(14, 7))
for ax, L in zip(axs, ["F.Cu", "B.Cu"]):
    draw.draw(b, ax, layers=[L], window=(x - half, y - half, x + half, y + half), labels=set(sys.argv[6:]) or {p.ref for p in b.parts if abs(p.x - x) < half and abs(p.y - y) < half})
    ax.set_title(L)
fig.savefig(out, dpi=80)
