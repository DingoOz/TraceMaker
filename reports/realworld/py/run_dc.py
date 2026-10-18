#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""run_dc.py BOARD LABEL ROUTED.kicad_pcb -> sim/results/BOARD/dc.LABEL.json: extracted copper of the power and LED-current nets, loaded with the
currents of cases.DC (assumed), ngspice operating point; per sink the voltage drop and the path resistance; IPC-2221 current limit of the
narrowest track of each net (external layer, 10 C rise)."""
import json, math, pathlib, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import cases, extract, kgeom, spice

board_name, label, path = sys.argv[1:4]
spec = cases.DC[board_name]
b = kgeom.load(path)
ids = {n: i for i, n in b.nets.items()}
nl = extract.Netlist()
jobs = []
for net, s in spec["nets"].items():
    jobs.append((net, s["source"], [(r, p, i) for r, p, i in s["sinks"]], None))
for net, hold_pin, i in spec["led"]:
    # source: the LED cathode at 3.0 V; sink: the other pin of the net (the LED driver) draws i
    jobs.append((net, (hold_pin[0], hold_pin[1], 3.0), None, i))
for net, *_ in jobs:
    extract.build_net(b, ids[net], nl, spec["layers"])
bad = spice.disconnected(nl, [j[0] for j in jobs])
if bad:
    print("DISCONNECTED NETS:", bad)
lines = [l for l in nl.lines if l[0] in "RL" or l.startswith("v")]  # copper R and L (DC: L is a short, C is open)
missing = []
probes = []
for k, (net, (sr, sp, sv), sinks, led_i) in enumerate(jobs):
    src = nl.pin_nodes.get((sr, sp, net))
    if not src:
        missing.append((net, sr, sp)); continue
    lines.append(f"Vsrc{k} {src} 0 {sv}")
    if sinks is None:
        others = [(key, v) for key, v in nl.pin_nodes.items() if key[2] == net and not (key[0] == sr and key[1] == sp)]
        sinks = [(key[0], key[1], led_i) for key, v in others[:1]]
    for j, (r, p, i) in enumerate(sinks):
        node = nl.pin_nodes.get((r, p, net))
        if not node:
            missing.append((net, r, p)); continue
        lines.append(f"Isk{k}_{j} {node} 0 {i}")
        probes.append((net, r, p, i, sv, node))
SC = spice.SCRATCH; SC.mkdir(parents=True, exist_ok=True)
cir = SC / f"dc_{board_name[:12]}_{label}.cir"; out = SC / f"dc_{board_name[:12]}_{label}.dat"
cir.write_text("\n".join(["* dc"] + lines + [".control", "op", "wrdata " + str(out) + " " + " ".join(f"v({p[5]})" for p in probes), "quit", ".endc", ".end"]) + "\n")
subprocess.run(["ngspice", "-b", str(cir)], capture_output=True)
import numpy as np
vals = np.loadtxt(out).reshape(-1)[1::2] if len(probes) else []
res = {}
for (net, r, p, i, sv, node), v in zip(probes, vals):
    d = res.setdefault(net, {"sinks": [], "worst_drop_mV": 0.0})
    drop = (sv - float(v)) * 1e3
    d["sinks"].append({"pin": f"{r}.{p}", "I_mA": i * 1e3, "drop_mV": round(drop, 3), "R_mohm": round(drop / (i * 1e3) * 1e3, 1) if i else None})
    d["worst_drop_mV"] = max(d["worst_drop_mV"], drop)
for net in res:
    tr = [t for t in b.tracks if t.net == ids[net]]
    wmin = min((t.w for t in tr), default=None)
    tot = sum(s["I_mA"] for s in res[net]["sinks"]) / 1e3 if net in [j[0] for j in jobs if j[2] is not None] else max(s["I_mA"] for s in res[net]["sinks"]) / 1e3
    # IPC-2221 external: I = 0.048 dT^0.44 A^0.725 (A in mil^2, 1 oz = 1.378 mil)
    amil2 = (wmin / 0.0254) * 1.378 if wmin else None
    imax = 0.048 * 10 ** 0.44 * amil2 ** 0.725 if amil2 else None
    res[net].update({"tracks": len(tr), "w_min_mm": wmin, "length_mm": round(sum(math.hypot(t.x2 - t.x1, t.y2 - t.y1) for t in tr), 1), "vias": len([v for v in b.vias if v.net == ids[net]]),
                     "I_total_A": tot, "IPC2221_Imax_A_at_wmin": round(imax, 2) if imax else None})
R = pathlib.Path("/home/dingo/Programming/TraceMaker/reports/realworld/sim/results") / board_name; R.mkdir(parents=True, exist_ok=True)
(R / f"dc.{label}.json").write_text(json.dumps({"missing": missing, "disconnected": bad, "nets": res}, indent=1))
print(board_name[:14], label, "missing pins:", missing[:4])
for net, d in sorted(res.items(), key=lambda x: -x[1]["worst_drop_mV"])[:8]:
    print(f"  {net:20s} worst drop {d['worst_drop_mV']:8.2f} mV  I_tot {d['I_total_A']*1e3:6.1f} mA  w_min {d['w_min_mm']} mm  IPC Imax {d['IPC2221_Imax_A_at_wmin']} A  length {d['length_mm']} mm")
