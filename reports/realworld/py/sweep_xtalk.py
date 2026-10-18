# SPDX-License-Identifier: GPL-3.0-or-later
"""How much does spacing matter? Peak crosstalk onto a quiet neighbour against the gap, for lengths of coupled run and edge rates, with the same
extraction + ngspice chain as the boards (4-layer stack, outer layer over a plane at 0.2104 mm; 2-layer, 1.6 mm over the far side).
Output: sim/results/xtalk_sweep.json and figs/xtalk_sweep.pdf"""
import json, math, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import numpy as np
import extract, kgeom, spice
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

ROOT = pathlib.Path("/home/dingo/Programming/TraceMaker/reports/realworld")
W = 0.15

def board(L, gap, w):
    pa = lambda ref, n, x, y, net, nm: kgeom.Pad(ref, "1", x, y, 0.3, 0.3, "rect", "smd", ["F.Cu"], net, nm)
    parts = [kgeom.Part("A1", "", "", 0, 0, 0, "F", [pa("A1", 1, 0, 0, 1, "A")]), kgeom.Part("A2", "", "", 0, 0, 0, "F", [pa("A2", 1, L, 0, 1, "A")]),
             kgeom.Part("B1", "", "", 0, 0, 0, "F", [pa("B1", 2, 0, w + gap, 2, "B")]), kgeom.Part("B2", "", "", 0, 0, 0, "F", [pa("B2", 2, L, w + gap, 2, "B")])]
    tracks = [kgeom.Track(0, 0, L, 0, w, "F.Cu", 1), kgeom.Track(0, w + gap, L, w + gap, w, "F.Cu", 2)]
    return kgeom.Board(parts, tracks, [], {1: "A", 2: "B"}, [], ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu"], {"trace_clearance": 0.2}, {})

def run(L, gap, tr, kind, layers):
    b = board(L, gap, W)
    nl = extract.Netlist()
    for n in (1, 2):
        extract.build_net(b, n, nl, layers)
    extract.couple(b, nl, {1, 2}, max_gap=1.0)
    extract.emit_caps(nl)
    lines = list(nl.lines)
    a1, a2, b1, b2 = (nl.pin_nodes[(r, "1", n)] for r, n in (("A1", "A"), ("A2", "A"), ("B1", "B"), ("B2", "B")))
    lines += [f"Vd d 0 PULSE(0 3.3 2n {tr} {tr} 40n 100n)", f"Rd d {a1} 40", f"Ca {a2} 0 5p"]
    if kind == "digital":   # quiet logic neighbour: held low through 40 ohm, 5 pF load
        lines += [f"Rb1 {b1} 0 40", f"Cb2 {b2} 0 5p"]
    else:                   # analog input: source impedance 1 kohm at the near end, 10 pF and 17.5 kohm at the far end
        lines += [f"Rb1 {b1} 0 1000", f"Cb2 {b2} 0 10p", f"Rb2 {b2} 0 17500"]
    probes = [({"ref": "B1", "pad": "1", "net": "B"}, b1), ({"ref": "B2", "pad": "1", "net": "B"}, b2)]
    out = spice.run_tran("xs", lines, probes, 30e-9, 0.02e-9, f"{L}_{gap}_{tr}_{kind}_{layers}")
    r = spice.analyse(out, probes, 0.0)
    return max(r[0]["pk"], r[1]["pk"]) * 1e3

res = {}
gaps = [0.09, 0.10, 0.127, 0.15, 0.20, 0.30, 0.40]
for layers in (4, 2):
    extract._single_cache.clear(); extract._pair_cache.clear()
    for kind in ("digital", "analog"):
        for tr in (1.5e-9, 0.5e-9):
            for L in (10.0, 25.0, 50.0):
                key = f"{layers}L_{kind}_tr{tr*1e9:.1f}ns_L{L:.0f}mm"
                res[key] = {"gap": gaps, "pk_mV": [run(L, g, tr, kind, layers) for g in gaps]}
                print(key, [round(x, 2) for x in res[key]["pk_mV"]], flush=True)
(ROOT / "sim/results/xtalk_sweep.json").write_text(json.dumps(res, indent=1))
fig, axs = plt.subplots(2, 2, figsize=(7.4, 5.6), sharex=True)
for i, layers in enumerate((4, 2)):
    for j, kind in enumerate(("digital", "analog")):
        ax = axs[i][j]
        for tr, ls in ((1.5, "-"), (0.5, "--")):
            for L, col in ((10, "#4575b4"), (25, "#f39c12"), (50, "#d73027")):
                d = res[f"{layers}L_{kind}_tr{tr:.1f}ns_L{L:.0f}mm"]
                ax.plot(d["gap"], d["pk_mV"], ls, color=col, label=f"{L} mm, {tr} ns" if i == 0 and j == 0 else None)
        for g, c in ((0.127, "#f39c12"), (0.20, "#1a9850")):
            ax.axvline(g, color=c, lw=0.7, ls=":")
        if kind == "digital":
            ax.axhline(800, color="k", lw=0.8, ls="-.")
            ax.text(0.095, 830, "LVTTL VIL 0.8 V", fontsize=6)
        ax.set_yscale("log")
        ax.set_title(f"{layers}-layer, {kind} victim", fontsize=8)
        ax.tick_params(labelsize=7)
        if j == 0: ax.set_ylabel("peak noise on the neighbour (mV)", fontsize=7)
        if i == 1: ax.set_xlabel("edge-to-edge gap (mm)", fontsize=7)
axs[0][0].legend(fontsize=6, ncol=2)
fig.tight_layout()
fig.savefig(ROOT / "figs/xtalk_sweep.pdf"); fig.savefig(ROOT / "figs/xtalk_sweep.png", dpi=150)
