#!/usr/bin/env python3
"""tighten_try.py BOARD RUN [route options...]: the finished closure run's copper put back on the board's OWN rules everywhere (no net relaxed): the router's
warm start legalises it (vertices moved, long tracks cut so they can bend), keeps the routes that are legal under those rules and routes the rest again.
Prints the open connections and the compromises (KiCad DRC added) before and after."""
import json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import closure, rules, ledger
board, run, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
d = closure.ROOT / "reports/realworld/boards" / board / run
src = (d / "baseline.kicad_pcb").read_text()
tag = "".join(c for c in "".join(extra) if c.isalnum() or c == ".")
out = d / ("tighten-" + tag)
rp, line = closure.route(src, out, d / "routed.kicad_pcb", 10_000_000, ["--legalize", "--soft-zones"] + extra, [])
j = closure.judge(rp, src, out)
base = json.loads((d / "ledger.json").read_text())
drc = json.loads((out / "routed.drc.json").read_text())
b0 = json.loads((d / "baseline.drc.json").read_text()) if (d / "baseline.drc.json").exists() else None
print(f"{board[:24]} {run}: before {base['unconnected']} open, {len(base['added'])} compromises; after tighten {j['open']} open, DRC violations {len(drc.get('violations', []))} (before: {len(json.loads((d/'routed.drc.json').read_text()).get('violations', [])) if (d/'routed.drc.json').exists() else '?'}); {line[:60]}")
