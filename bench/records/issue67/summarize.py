#!/usr/bin/env python3
"""Tables for bench/records/issue67 from bench/pair_eval.py --json files: per option set the pairs coupled >= 80 % / >= 50 % /
at all, the median coupled share, connections routed and KiCad errors added; then per board the coupled share of each pair.

  summarize.py RESULTS.json [RESULTS.json ...]
"""
import json
import statistics
import sys

rows = []
for f in sys.argv[1:]:
    rows += json.load(open(f))["rows"]
modes = list(dict.fromkeys(r["mode"] for r in rows))
boards = list(dict.fromkeys(r["board"] for r in rows))
by = {(r["board"], r["mode"]): r for r in rows}

print("| Options | Pairs | >= 80 % | >= 50 % | > 0 % | Median share | Mean share | Routed | KiCad errors added | Clean boards |")
print("|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|")
for m in modes:
    sh, routed, conns, added, clean = [], 0, 0, 0, 0
    for b in boards:
        r = by.get((b, m))
        if not r:
            continue
        sh += [p["coupled_share"] for p in r["pairs"]]
        routed += r.get("routed") or 0
        conns += r.get("connections") or 0
        added += sum(r.get("added_errors", {}).values())
        clean += 1 if r.get("clean") else 0
    print(f"| {m} | {len(sh)} | {sum(s >= 0.8 for s in sh)} | {sum(s >= 0.5 for s in sh)} | {sum(s > 0 for s in sh)} | "
          f"{100 * statistics.median(sh):.0f} % | {100 * statistics.mean(sh):.1f} % | {routed} / {conns} | {added} | {clean} |")
print()
print("| Board | " + " | ".join(modes) + " |")
print("|---|" + "---|" * len(modes))
for b in boards:
    cells = []
    for m in modes:
        r = by.get((b, m))
        if not r:
            cells.append("–")
            continue
        sh = " ".join(f"{100 * p['coupled_share']:.0f}" for p in r["pairs"])
        cells.append(f"{sh} ({r.get('routed')}/{r.get('connections')}, +{sum(r.get('added_errors', {}).values())})")
    print(f"| {b} | " + " | ".join(cells) + " |")
