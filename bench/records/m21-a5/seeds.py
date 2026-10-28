#!/usr/bin/env python3
"""M21 A5 (doc 18): #24's critical-net options over five seeds on the plane tier. Per arm, each measure's mean and range over the
seeds that have finished (bench/results/a5-<arm>-s<seed>/boards/*.route.json), and each seed's row in --rows.

    seeds.py [--rows] [ARM ...]      (default: every arm found)"""
import json
import pathlib
import statistics
import sys

root = pathlib.Path(__file__).resolve().parents[3] / "bench" / "results"
rows = "--rows" in sys.argv
want = [a for a in sys.argv[1:] if not a.startswith("--")]


def measure(run):
    m = dict(nets=0, routed=0, vias=0, crossings=0, splits=0, missing=0, missing_before=0, conns=0, total=0, added=0, unconnected=0)
    for f in sorted((run / "boards").glob("*.route.json")):
        j = json.loads(f.read_text())
        m["conns"] += j.get("routed", 0)
        m["total"] += j.get("connections", 0)
        cn = j.get("critical_nets")
        if not cn:
            continue
        t = cn.get("totals", {})
        m["missing"] += t.get("missing_stitches", 0)
        m["missing_before"] += t.get("missing_stitches_before_stitching", t.get("missing_stitches", 0))
        for n in cn["nets"]:
            m["nets"] += 1
            if n["unrouted"] == 0:
                m["routed"] += 1
                m["vias"] += n["vias"]
                m["crossings"] += n.get("void_crossings", 0)
                m["splits"] += n.get("splits", 0)
    for line in (run / "boards.jsonl").read_text().splitlines():
        b = json.loads(line)
        m["added"] += bool(b.get("added_any"))
        m["unconnected"] += b.get("unconnected_after") or 0
    return m


arms = {}
for d in sorted(root.glob("a5-*-s*")):
    if not (d / "summary.json").exists():
        continue  # still running
    arm, seed = d.name[3:].rsplit("-s", 1)
    if want and arm not in want:
        continue
    arms.setdefault(arm, {})[int(seed)] = measure(d)

keys = ["routed", "vias", "crossings", "splits", "missing", "conns", "unconnected", "added"]
heads = ["critical nets routed", "their vias", "void crossings", "splits", "unstitched changes", "connections routed (all nets)",
         "KiCad unconnected", "boards with added errors"]
order = ["base", "first", "split", "via", "splitvia", "firstsplit", "si", "sistitch"]
print("| arm | seeds | " + " | ".join(heads) + " |")
print("|---|--:|" + "--:|" * len(keys))
for arm in sorted(arms, key=lambda a: order.index(a) if a in order else 99):
    s = arms[arm]
    cells = []
    for k in keys:
        v = [s[x][k] for x in sorted(s)]
        mean = statistics.fmean(v)
        cells.append(f"{mean:.1f} ({min(v)}–{max(v)})" if len(v) > 1 else f"{v[0]}")
    print(f"| {arm} | {len(s)} | " + " | ".join(cells) + " |")
if rows:
    print()
    print("| arm | seed | critical nets | " + " | ".join(heads) + " |")
    print("|---|--:|--:|" + "--:|" * len(keys))
    for arm in sorted(arms, key=lambda a: order.index(a) if a in order else 99):
        for seed, m in sorted(arms[arm].items()):
            print(f"| {arm} | {seed} | {m['nets']} | " + " | ".join(str(m[k]) for k in keys) + " |")
