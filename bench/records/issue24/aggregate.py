#!/usr/bin/env python3
"""Per run: the critical-net report of every board's route summary (bench/results/<run>/boards/*.route.json), summed.
usage: aggregate.py RUN [RUN ...]    (runs made with --critical-auto, alone or with the rules: the report is there either way)"""
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[3] / "bench" / "results"
print("| run | boards with critical nets | critical nets | routed | vias | void crossings | splits | routed connections (all nets) |")
print("|---|---|---|---|---|---|---|---|")
for run in sys.argv[1:]:
    boards = nets = routed = vias = cross = splits = conns = total = 0
    for f in sorted((root / run / "boards").glob("*.route.json")):
        j = json.loads(f.read_text())
        conns += j.get("routed", 0)
        total += j.get("connections", 0)
        cn = j.get("critical_nets")
        if not cn or not cn["nets"]:
            continue
        boards += 1
        for n in cn["nets"]:
            if "unrouted" not in n:
                sys.exit(f"{run}: {f.name} has no `unrouted` field (a run made with an older binary): its nets would all count as routed")
            nets += 1
            if n["unrouted"] == 0:
                routed += 1
                vias += n["vias"]
                cross += n.get("void_crossings", 0)
                splits += n.get("splits", 0)
    print(f"| {run} | {boards} | {nets} | {routed} | {vias} | {cross} | {splits} | {conns} / {total} |")
