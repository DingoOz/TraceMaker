#!/usr/bin/env python3
"""A fair comparison of the critical-net reports of two runs: only the nets that are fully routed in BOTH are compared (a net that one run could not route has no vias
and no crossings to count). Per variant: how many critical nets each run routed, and over the common ones the vias, the void crossings (via antipads included)
and the splits of the baseline and of the variant. Needs route summaries with the `unrouted` field (bench/results/<run>/boards/*.route.json).
usage: compare_nets.py BASE VARIANT [VARIANT ...]"""
import json
import pathlib
import sys

root = pathlib.Path(__file__).resolve().parents[3] / "bench" / "results"


def load(run):
    out, conns, total = {}, 0, 0
    for f in sorted((root / run / "boards").glob("*.route.json")):
        j = json.loads(f.read_text())
        conns += j.get("routed", 0)
        total += j.get("connections", 0)
        for n in (j.get("critical_nets") or {}).get("nets", []):
            if "unrouted" not in n:
                sys.exit(f"{run}: {f.name} has no `unrouted` field: run made with an older binary")
            out[(f.name, n["name"])] = n
    return out, conns, total


base, bconns, btotal = load(sys.argv[1])
print(f"baseline {sys.argv[1]}: {sum(1 for n in base.values() if n['unrouted'] == 0)} of {len(base)} critical nets routed, {bconns} of {btotal} connections")
print("| variant | critical nets routed | connections routed | common nets | vias (base -> variant) | void crossings | splits |")
print("|---|---|---|---|---|---|---|")
for run in sys.argv[2:]:
    var, conns, total = load(run)
    common = [k for k in base if k in var and base[k]["unrouted"] == 0 and var[k]["unrouted"] == 0]
    s = lambda d, key: sum(d[k].get(key, 0) for k in common)
    routed = sum(1 for n in var.values() if n["unrouted"] == 0)
    print(f"| {run} | {routed} / {len(var)} | {conns} / {total} | {len(common)} | {s(base, 'vias')} -> {s(var, 'vias')} | {s(base, 'void_crossings')} -> {s(var, 'void_crossings')} | {s(base, 'splits')} -> {s(var, 'splits')} |")
