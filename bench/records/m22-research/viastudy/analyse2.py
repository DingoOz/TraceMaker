#!/usr/bin/env python3
"""Detours of the rescued routes (BIG/CHEAP/V*), and the FB/W pass (exact vs fast bends vs the winning variant), from out/*.json."""
import json, math, pathlib, statistics, sys
from collections import Counter
OUT = pathlib.Path(__file__).parent / "out"
boards = sys.argv[1:]
print("| board | exp | rescued | median vias | median detour (route / straight) | A why of those (first clause) |")
print("|---|---|--:|--:|--:|---|")
for b in boards:
    d = json.loads((OUT / f"{b}.json").read_text())
    for k in ["V2", "T2", "BIG", "CHEAP"]:
        vs, dr, why = [], [], Counter()
        for x in d["connections"]:
            e = x["exp"]
            if e["A"]["routed"] or not e.get(k, {}).get("routed"): continue
            r = e[k]
            if "len_by_layer" not in r: continue
            if len(x["ends"]) == 2:
                (ax, ay), (bx, by) = x["ends"][0]["pos"], x["ends"][1]["pos"]
                s = math.hypot(ax - bx, ay - by)
                if s > 0.5: dr.append(sum(r["len_by_layer"].values()) / s)
            vs.append(r.get("vias", 0))
            why[e["A"].get("why", "").split(";")[0][:40]] += 1
        if vs:
            print(f"| {b} | {k} | {len(vs)} | {statistics.median(vs)} | {statistics.median(dr):.2f} | {dict(why.most_common(3))} |")
print()
print("| board | sampled | A exact bends | FB fast bends | W winning variant | A only | FB only | W only |")
print("|---|--:|--:|--:|--:|--:|--:|--:|")
for b in boards:
    p = OUT / f"{b}.fb.json"
    if not p.exists(): continue
    d = json.loads(p.read_text())
    c = Counter()
    for x in d["connections"]:
        e = x["exp"]; a, f, w = (e.get(k, {}).get("routed", False) for k in ("A", "FB", "W"))
        c["n"] += 1; c["A"] += a; c["FB"] += f; c["W"] += w
        c["Ao"] += a and not f and not w; c["Fo"] += f and not a and not w; c["Wo"] += w and not a and not f
    print(f"| {b} | {c['n']} | {c['A']} | {c['FB']} | {c['W']} | {c['Ao']} | {c['Fo']} | {c['Wo']} |")
