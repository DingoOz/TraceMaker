#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""run_sim.py BOARD CASE LAYOUT_LABEL ROUTED.kicad_pcb [--gap-floor 0.2]  ->  sim/results/BOARD/CASE.LABEL.json (+ .npz waveforms)"""
import json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import numpy as np
import cases, kgeom, spice

board_name, case_name, label, path = sys.argv[1:5]
floor = float(sys.argv[sys.argv.index("--gap-floor") + 1]) if "--gap-floor" in sys.argv else 0.0
case = cases.CASES[board_name][case_name]
b = kgeom.load(path)
lines, nl, pairs, probes = spice.build(b, case["layers"], case, gap_floor=floor)
tag = label + (f"_gap{floor}" if floor else "")
out = spice.run_tran(f"{board_name[:12]}_{case_name}", lines, probes, case["stop"], case["step"], tag)
res = spice.analyse(out, probes, case["skip"])
R = pathlib.Path("/home/dingo/Programming/TraceMaker/reports/realworld/sim/results") / board_name
R.mkdir(parents=True, exist_ok=True)
bad = spice.disconnected(nl, case["nets"])
if bad:
    print("DISCONNECTED NETS:", bad)
summary = {"disconnected": bad, "board": board_name, "case": case_name, "layout": tag, "elements": len(lines),
           "pairs": [{"nets": [b.nets[x] for x in k], "run_mm": round(v["run"], 2), "cm_pF": round(v["cm"] * 1e12, 3), "gap_min_mm": round(v["gap_min"], 3)} for k, v in pairs.items()],
           "nets": {n: nl.stats[n] for n in nl.stats},
           "probes": [{k: r[k] for k in ("label", "net", "base", "pk", "pk_pos", "pk_neg", "rms")} for r in res]}
(R / f"{case_name}.{tag}.json").write_text(json.dumps(summary, indent=1))
np.savez_compressed(R / f"{case_name}.{tag}.npz", **{f"t{i}": r["t"] for i, r in enumerate(res)}, **{f"v{i}": r["v"] for i, r in enumerate(res)})
print(board_name[:14], case_name, tag, "elements", len(lines), "pairs", len(pairs))
for r in res:
    print(f"   {r['label'][:44]:44s} pk {r['pk']*1e3:8.2f} mV  (+{r['pk_pos']*1e3:7.2f} / {r['pk_neg']*1e3:7.2f})")
