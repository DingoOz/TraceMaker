# SPDX-License-Identifier: GPL-3.0-or-later
"""Worst-case crosstalk bound for EVERY compromised pair of tracks of a layout (not only the simulated nets): each pair (gap under 0.2 mm, run side by
side) is looked up in the field-solver + ngspice sweep (sim/results/xtalk_sweep.json: 3.3 V aggressor, 1.5 ns edges, 40 ohm; quiet logic victim),
assuming the worse case that both nets are active signals. Writes tex/gen/bound_<key>_<layout>.tex and prints the summary."""
import json, pathlib, sys
import numpy as np
sys.path.insert(0, str(pathlib.Path(__file__).parent))
RW = pathlib.Path("/home/dingo/Programming/TraceMaker/reports/realworld")
sw = json.load(open(RW / "sim/results/xtalk_sweep.json"))

def noise(layers, L, gap, tr="1.5", kind="digital"):
    lens = [10.0, 25.0, 50.0]
    vals = []
    for ln in lens:
        d = sw[f"{layers}L_{kind}_tr{tr}ns_L{ln:.0f}mm"]
        vals.append(np.interp(np.log(gap), np.log(d["gap"]), d["pk_mV"]))
    return float(np.interp(L, [0.0] + lens, [0.0] + vals))

def esc(s):
    return s.replace("_", "\\_").replace("&", "\\&").replace("#", "\\#").replace("%", "\\%")

out = {}
for key, name, layers, lays in (("T", "kitspace_teensy-fx", 4, ["ladder-A", "ladder-B"]), ("S", "Hardware_Playground_serial_gw_ATMEGA328P", 2, ["ladder-A", "ladder-C"])):
    for lay in lays:
        j = json.load(open(RW / "boards" / name / lay / "ledger.json"))
        # merge pairs of the same two nets: run adds, gap = smallest
        agg = {}
        for p in j["near"]:
            if p["kind"] != "track-track" or p["run"] < 0.5:
                continue
            k = tuple(sorted(p["nets"]))
            a = agg.setdefault(k, {"run": 0.0, "gap": 9.0, "n": 0})
            a["run"] += p["run"]; a["gap"] = min(a["gap"], max(p["gap"], 0.09)); a["n"] += 1
        rows = []
        for (n1, n2), a in agg.items():
            L = min(a["run"], 50.0)
            nz = noise(layers, L, a["gap"])
            nz0 = noise(layers, L, 0.2)
            rows.append((nz, nz0, n1, n2, a))
        rows.sort(key=lambda r: -r[0])
        worst = rows[0] if rows else None
        out[f"{key}_{lay}"] = {"pairs": len(rows), "worst_mV": worst[0] if worst else 0, "worst_at_0.2_mV": worst[1] if worst else 0, "max_increase_mV": max((r[0] - r[1] for r in rows), default=0),
                               "worst_pair": [worst[2], worst[3], worst[4]] if worst else None}
        tex = ["\\begin{tabular}{llrrrr}", "\\toprule", "net & net & run (mm) & smallest gap (mm) & bound (mV) & at 0.2 mm (mV) \\\\", "\\midrule"]
        for nz, nz0, n1, n2, a in rows[:8]:
            tex.append(f"\\texttt{{{esc(n1)}}} & \\texttt{{{esc(n2)}}} & {a['run']:.1f} & {a['gap']:.3f} & {nz:.0f} & {nz0:.0f} \\\\")
        tex += ["\\bottomrule", "\\end{tabular}"]
        (RW / f"tex/gen/bound_{key}_{lay[-1]}.tex").write_text("\n".join(tex) + "\n")
        print(key, lay, "compromised track pairs:", len(rows), "worst bound", round(out[f'{key}_{lay}']['worst_mV']), "mV (", round(out[f'{key}_{lay}']['worst_at_0.2_mV']), "at 0.2 mm )", "largest increase", round(out[f'{key}_{lay}']['max_increase_mV']), "mV")
json.dump(out, open(RW / "sim/results/bound_pairs.json", "w"), indent=1)
