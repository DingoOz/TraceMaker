#!/usr/bin/env python3
"""Per-board table of bench runs (boards.jsonl): connections routed (the router's count, as in doc 10 §11.3) and KiCad-clean boards.

usage: compare.py RUN_DIR [RUN_DIR ...]   (first run is the reference for better/worse)
"""
import json, pathlib, sys

runs = [pathlib.Path(r) for r in sys.argv[1:]]
data = {}
for r in runs:
    data[r.name] = {}
    for line in open(r / "boards.jsonl"):
        d = json.loads(line)
        data[r.name][d["board"]] = d
names = [r.name for r in runs]
boards = sorted(set.intersection(*[set(d) for d in data.values()]), key=lambda b: data[names[0]][b]["connections"])


def routed(d):
    return d.get("routed", 0)


print("| board | connections | " + " | ".join(names) + " |")
print("|---|--:|" + "--:|" * len(names))
tot = {n: 0 for n in names}
clean = {n: 0 for n in names}
better = {n: [0, 0] for n in names}
for b in boards:
    cells = []
    for n in names:
        d = data[n][b]
        v = routed(d)
        tot[n] += v
        ok = d.get("clean") and not d.get("added_errors")
        clean[n] += 1 if ok else 0
        ref = routed(data[names[0]][b])
        if v > ref: better[n][0] += 1
        if v < ref: better[n][1] += 1
        cells.append(f"{v}{' **clean**' if ok else ''}{' +err' if d.get('added_errors') else ''}")
    print(f"| {b} | {data[names[0]][b]['connections']} | " + " | ".join(cells) + " |")
print(f"| **{len(boards)} boards** | {sum(data[names[0]][b]['connections'] for b in boards)} | " +
      " | ".join(f"**{tot[n]}** ({tot[n] - tot[names[0]]:+d}; {better[n][0]} better, {better[n][1]} worse; {clean[n]} clean)" for n in names) + " |")
