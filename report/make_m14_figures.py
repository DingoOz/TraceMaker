#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Figures for the BGA breakout section of report/report.tex (milestone M14, docs/16-bga-breakout-plan.md).

The numbers are the measured results recorded in docs/16 section 10-11 (bench/results/srj19-*, srj20-*, fan31-*).

  build/report-venv/bin/python report/make_m14_figures.py
"""
import pathlib

import matplotlib

matplotlib.use("pdf")
import matplotlib.pyplot as plt  # noqa: E402
from PIL import Image  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "report/figures"
C_TM, C_TS, C_OLD = "#2a78d6", "#eb6834", "#a9c4ea"
INK, GRID = "#0b0b0b", "#e4e3df"

plt.rcParams.update({"font.size": 8.5, "axes.titlesize": 9, "axes.labelsize": 8.5, "axes.edgecolor": INK,
                     "axes.spines.top": False, "axes.spines.right": False, "pdf.fonttype": 42})

# (set, boards, TraceMaker before M14, TraceMaker now, tscircuit)
rows = [("srj19\n(200 BGA breakouts)", 200, 159, 191, 184),
        ("srj20\n(200, missing balls)", 200, 100, 160, 157),
        ("fanout31\n(72 SoC fan-outs)", 72, 0, 60, 48)]
fig, ax = plt.subplots(figsize=(6.4, 2.9))
w = 0.26
for i, (name, n, before, now, ts) in enumerate(rows):
    for j, (v, c, lab) in enumerate([(before, C_OLD, "TraceMaker before M14"), (now, C_TM, "TraceMaker now"),
                                      (ts, C_TS, "tscircuit router")]):
        ax.bar(i + (j - 1) * w, 100 * v / n, w * 0.92, color=c, label=lab if i == 0 else None)
        ax.text(i + (j - 1) * w, 100 * v / n + 1.5, f"{v}" if not (i == 2 and j == 0) else "n/a", ha="center",
                fontsize=7.5, color=INK)
ax.set_xticks(range(3), [r[0] for r in rows])
ax.set_ylabel("boards or cases solved (%)")
ax.set_ylim(0, 108)
ax.yaxis.grid(True, color=GRID)
ax.set_axisbelow(True)
ax.legend(frameon=False, ncol=3, loc="upper center", bbox_to_anchor=(0.5, 1.16), fontsize=8)
fig.tight_layout()
fig.savefig(OUT / "m14_gates.pdf")
plt.close(fig)

# srj20 gap boards (57): clean share against the direction penalty, and routed share
dp = [0, 0.15, 0.3, 0.5, 1.0]
clean = [28, 30, 40, 35, 30]
routed = [87.7, 89.0, 89.8, 90.7, 91.0]
fig, ax = plt.subplots(figsize=(4.4, 2.6))
ax.plot(dp, clean, "o-", color=C_TM, label="boards clean (%)")
ax.plot(dp, routed, "s--", color=C_TS, label="connections routed (%)")
ax.set_xlabel("direction penalty (pitches per step against the layer's direction)")
ax.set_ylabel("%")
ax.yaxis.grid(True, color=GRID)
ax.set_axisbelow(True)
ax.legend(frameon=False, fontsize=8, loc="center right")
fig.tight_layout()
fig.savefig(OUT / "m14_dirpen.pdf")
plt.close(fig)

# srj20 deficit by board size: boards solved by tscircuit / TraceMaker (first profile, 50 M)
bins = ["<40", "40-49", "50-59", "60-79", ">=80"]
ts = [99, 29, 21, 7, 1]
tm = [99, 21, 4, 0, 0]
fig, ax = plt.subplots(figsize=(4.4, 2.6))
x = range(len(bins))
ax.bar([i - 0.2 for i in x], ts, 0.38, color=C_TS, label="tscircuit solves")
ax.bar([i + 0.2 for i in x], tm, 0.38, color=C_TM, label="TraceMaker (one variant, 50 M)")
ax.set_xticks(list(x), bins)
ax.set_xlabel("connections per board (srj20)")
ax.set_ylabel("boards solved")
ax.yaxis.grid(True, color=GRID)
ax.set_axisbelow(True)
ax.legend(frameon=False, fontsize=8)
fig.tight_layout()
fig.savefig(OUT / "m14_size.pdf")
plt.close(fig)

# captures: shrink the 3D render for the PDF
for src, dst in [("kicad-3d-srj19-sample196-copper.png", "m14_kicad3d.jpg")]:
    im = Image.open(ROOT / "report/captures" / src).convert("RGB")
    im.thumbnail((1800, 1800))
    im.save(OUT / dst, quality=88)
for src, dst in [("srj19-sample196-68conn-routed.png", "m14_srj19.png"), ("fanout31-am62l-133conn-routed.png", "m14_am62l.png")]:
    Image.open(ROOT / "report/captures" / src).save(OUT / dst)
print("ok")
