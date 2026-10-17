#!/usr/bin/env python3
"""shove_try.py BOARD RUN [route options...]: from the best state of a finished closure run, one extra round with the router's gridless compaction (--relax:
route tighter, legalise up to the real rules = tracks and vias pushed apart), judged by KiCad. Prints the open count before and after."""
import json, pathlib, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import closure, rules, pcbtext
board, run, extra = sys.argv[1], sys.argv[2], sys.argv[3:]
R = closure.ROOT / "reports/realworld/boards" / board
d = R / run
st = json.loads((d / "closure.json").read_text())
src = (d / "baseline.kicad_pcb").read_text()
tiers = dict(st["tiers"])
nets_all = rules.all_nets(src)
assign = {n: t for n, t in st["assign"].items()}
text = rules.apply_nets(src, assign, tiers)
strict = [n for n in nets_all if n not in assign]
best_open = min(r["open"] for r in st["rounds"] if r["kind"] in ("route", "effort"))
best = next(p for p in sorted(d.glob("*/routed_relaxed.kicad_pcb"), key=lambda p: p.stat().st_mtime) if False) if False else None
# the routed copper of the best state: the run's final routed.kicad_pcb carries the board's own rules; the relaxed copy is the same copper
warm = d / "routed.kicad_pcb"
out = d / ("shove-try" + "".join(c for c in "".join(extra) if c.isalnum() or c == "."))  # one directory per option set
rp, line = closure.route(text, out, warm, 20_000_000, ["--relax", "--rounds", "6", "--round-work", "4000000", "--soft-zones"] + extra, strict)
j = closure.judge(rp, src, out)
print(f"{board[:24]} {run}: best {best_open} open -> shove {j['open']} open; {line[:70]}")
