#!/usr/bin/env python3
"""False-positive sweep for `tracemaker cuts` (doc 05 §67): every board a benchmark run judged clean (no error added by
routing, nothing unconnected) satisfies the cut condition, so any over-full cut with a wire in it, or a via without
room, on such a board is a bug in the test. Over-full cuts between two pads with no wire are the board's own spacing.

usage: legal_sweep.py TRACEMAKER RUN_DIR [RUN_DIR ...] > sweep.tsv
"""
import json, pathlib, subprocess, sys
from concurrent.futures import ThreadPoolExecutor

tm = sys.argv[1]


def clean_boards(run):
    run = pathlib.Path(run)
    for line in open(run / "boards.jsonl"):
        d = json.loads(line)
        if d.get("exit") == 0 and not d.get("added_errors") and d.get("unconnected_after") == 0 and d.get("routed") == d.get("connections"):
            pcb = run / "boards" / f"{d['board']}.kicad_pcb"
            if pcb.exists():
                yield run.name, d["board"], pcb


def check(item):
    run, board, pcb = item
    out = pcb.with_suffix(".cuts.json")
    r = subprocess.run([tm, "cuts", str(pcb), "--json", str(out)], capture_output=True, text=True, timeout=600)
    if r.returncode != 0:
        return f"{run}\t{board}\tERROR\t{(r.stderr or r.stdout)[-200:]!r}"
    j = json.load(open(out))
    out.unlink()
    wired = [c for c in j["overfull"] if c["wires"]]
    own = len(j["overfull"]) - len(wired)
    worst = max((c["need_mm"] - c["gap_mm"] for c in wired), default=0)
    return f"{run}\t{board}\t{j['cuts']}\t{len(wired)}\t{own}\t{len(j['vias_without_room'])}\t{j['s_star']:.3f}\t{worst:.4f}"


items = [x for run in sys.argv[2:] for x in clean_boards(run)]
print("run\tboard\tcuts\toverfull_with_wires\toverfull_pad_pad\tvias_without_room\ts_star\tworst_mm")
with ThreadPoolExecutor(6) as ex:
    for line in ex.map(check, items):
        print(line, flush=True)
