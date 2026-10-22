#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""priority_try.py BOARD RUN WORK NETS_COMMA [route options...]: the board of a finished closure run (its tiers, hard pours, part moves) routed FROM SCRATCH (no warm start) with the
given nets routed first (--first-nets), judged by KiCad. One experiment: does routing the stuck nets first, with everything else around them, close them?"""
import json, pathlib, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import closure, rules, pcbtext
board, run, work, nets = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4]
extra = sys.argv[5:]
R = closure.ROOT / "reports/realworld/boards" / board
c = json.loads((R / run / "closure.json").read_text())
src = ((R / "planes.kicad_pcb")).read_text()
for ref, (dx, dy) in c["moves"].items():
    src = pcbtext.move_part(src, ref, dx, dy)
closure.HARD.clear(); closure.HARD.update(c.get("hard", []))
tiers = c["tiers"]
text = rules.apply_nets(pcbtext.remove_zones(src, closure.HARD), c["assign"], tiers)
nets_all = rules.all_nets(src)
strict = [n for n in nets_all if n not in c["assign"]]
out = R / (run + "-prio")
closure.THREADS = 8
rp, line = closure.route(text, out, None, int(work), extra + ["--first-nets", nets, "--soft-zones"], strict)
j = closure.judge(rp, src, out)
print(f"{board[:24]} {run}: first-nets {nets[:60]}: {line[:70]}; KiCad {j['open']} open, {j['violations']} violations")
