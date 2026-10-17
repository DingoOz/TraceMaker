# SPDX-License-Identifier: GPL-3.0-or-later
"""Validation of the extraction + coupling + ngspice chain on two straight coupled microstrips (4-layer stack, h = 0.2104 mm, er 4.5):
near-end crosstalk of matched, weakly coupled lines against Kb = (Cm/C + Lm/L)/4 (saturated NEXT), and the convergence with section length."""
import math, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import numpy as np
import extract, fd2d, kgeom, spice

W, GAP, LEN, H, T = 0.15, 0.127, 120.0, 0.2104, 0.035

def board():
    pa = lambda ref, n, x, y, net, nm: kgeom.Pad(ref, "1", x, y, 0.3, 0.3, "rect", "smd", ["F.Cu"], net, nm)
    parts = [kgeom.Part("A1", "", "", 0, 0, 0, "F", [pa("A1", 1, 0, 0, 1, "A")]), kgeom.Part("A2", "", "", 0, 0, 0, "F", [pa("A2", 1, LEN, 0, 1, "A")]),
             kgeom.Part("B1", "", "", 0, 0, 0, "F", [pa("B1", 2, 0, W + GAP, 2, "B")]), kgeom.Part("B2", "", "", 0, 0, 0, "F", [pa("B2", 2, LEN, W + GAP, 2, "B")])]
    tracks = [kgeom.Track(0, 0, LEN, 0, W, "F.Cu", 1), kgeom.Track(0, W + GAP, LEN, W + GAP, W, "F.Cu", 2)]
    return kgeom.Board(parts, tracks, [], {1: "A", 2: "B"}, [], ["F.Cu", "In1.Cu", "In2.Cu", "B.Cu"], {"trace_clearance": 0.2}, {})

# theory
cond = [(-W - GAP / 2, -GAP / 2, H, H + T), (GAP / 2, GAP / 2 + W, H, H + T)]
C = fd2d.cap_matrix(cond, [(0, H, 4.5)], [(0, 0.01)], dx=0.01, ytop=H + T + 3)
L = fd2d.inductance_matrix(cond, [(0, H, 4.5)], [(0, 0.01)], dx=0.01, ytop=H + T + 3)
Cself, Cm = C[0, 0] + C[0, 1], -C[0, 1]
Z0 = math.sqrt(L[0, 0] / Cself)   # approximate: self L over the self-to-ground C
Kb = 0.25 * (Cm / (Cself + Cm) + L[0, 1] / L[0, 0])
print(f"field solver: Z0 about {Z0:.1f} ohm, Cm/C = {Cm / (Cself + Cm):.3f}, Lm/L = {L[0, 1] / L[0, 0]:.3f}, Kb = {Kb:.4f}")
v = 3.3
for sec in (4.0, 2.0, 1.0, 0.5):
    extract.SECTION = sec
    extract._single_cache.clear()
    b = board()
    nl = extract.Netlist()
    for n in (1, 2):
        extract.build_net(b, n, nl, 4)
    pairs = extract.couple(b, nl, {1, 2}, max_gap=0.6)
    extract.emit_caps(nl)
    lines = list(nl.lines)
    a1, a2, b1, b2 = nl.pin_nodes[("A1", "1", "A")], nl.pin_nodes[("A2", "1", "A")], nl.pin_nodes[("B1", "1", "B")], nl.pin_nodes[("B2", "1", "B")]
    # matched: driver Z0 at A1, Z0 at A2, B terminated Z0 at both ends
    lines += [f"Vd d 0 PULSE(0 {v} 1n 0.4n 0.4n 20n 60n)", f"Rd d {a1} {Z0:.4g}", f"Ra2 {a2} 0 {Z0:.4g}", f"Rb1 {b1} 0 {Z0:.4g}", f"Rb2 {b2} 0 {Z0:.4g}"]
    probes = [({"ref": "B1", "pad": "1", "net": "B", "label": "near end"}, b1), ({"ref": "B2", "pad": "1", "net": "B", "label": "far end"}, b2), ({"ref": "A2", "pad": "1", "net": "A", "label": "through"}, a2)]
    out = spice.run_tran("validate", lines, probes, 20e-9, 0.01e-9, f"sec{sec}")
    res = spice.analyse(out, probes, 0.0)
    near, far, thr = res[0]["pk"], res[1]["pk"], res[2]["pk"]
    tdel = LEN * 1e-3 * math.sqrt(4.5 * 0.7) / 3e8
    print(f"section {sec:4.1f} mm: elements {len(lines):5d}  NEXT {near * 1e3:7.2f} mV  FEXT {far * 1e3:7.2f} mV  through {thr:5.3f} V | theory NEXT (saturated) {Kb * v / 2 * 1e3:6.1f} mV")
