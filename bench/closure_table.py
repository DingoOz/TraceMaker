#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Placement closure results (doc 04 §11, D190): per board, connections left open in each run, "complete"
(KiCad counts nothing unconnected) and "clean" (complete and no DRC error added, as bench/run.py judges it),
plus what the closure loop did (trials, kept changes, moved parts, arms) from the placed boards' reports.

  bench/closure_table.py RUN [RUN ...] [--json out.json]

RUN is a bench/results/<run-id> directory written by bench/run.py; runs made with --place closure also give the loop's
statistics. Boards are matched by name; totals are over the boards every run has.
"""
import argparse
import json
import pathlib
import sys


def load(run: pathlib.Path) -> dict:
    out = {}
    for line in (run / "boards.jsonl").read_text().splitlines():
        j = json.loads(line)
        rec = {"open": j.get("connections", 0) - j.get("routed", 0), "clean": bool(j.get("clean")), "place_s": j.get("place_s"),
               "complete": j.get("unconnected_after") == 0, "place_added": j.get("place_added") or {}}
        js = run / "boards" / f"{j['board']}.placed.json"
        if js.exists():
            p = json.loads(js.read_text())
            c = p.get("closure") or {}
            rec.update(trials=c.get("routes"), kept=c.get("accepted"), stop=c.get("stop"), moved=p.get("moved"), arms=c.get("arms"),
                       changes=[f"{t['arm']}: {t['what']} ({t['before']} -> {t['after']})" for t in c.get("trials", []) if t["accepted"]])
        out[j["board"]] = rec
    return out


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--json")
    a = ap.parse_args()
    runs = {pathlib.Path(r).name: load(pathlib.Path(r)) for r in a.runs}
    common = sorted(set.intersection(*(set(d) for d in runs.values())))
    names = list(runs)
    print("| Board | " + " | ".join(names) + " |")
    print("|---|" + "--:|" * len(names))
    for b in common:
        cells = []
        for n in names:
            r = runs[n][b]
            tag = " clean" if r["clean"] else " complete" if r["complete"] else ""
            bad = f" (+{sum(r['place_added'].values())} placement DRC)" if r["place_added"] else ""
            cells.append(f"{r['open']}{tag}{bad}")
        print(f"| {b} | " + " | ".join(cells) + " |")
    print("| **Total open / complete / clean** | " + " | ".join(
        f"{sum(runs[n][b]['open'] for b in common)} / {sum(runs[n][b]['complete'] for b in common)} / {sum(runs[n][b]['clean'] for b in common)}"
        for n in names) + " |")
    for n in names:
        arms: dict = {}
        for b in common:
            for k, v in (runs[n][b].get("arms") or {}).items():
                s = arms.setdefault(k, [0, 0])
                s[0] += v["pulls"]
                s[1] += v["wins"]
        if arms:
            print(f"\n{n}: arm trials/wins " + ", ".join(f"{k} {v[0]}/{v[1]}" for k, v in arms.items()))
            for b in common:
                r = runs[n][b]
                if r.get("trials") is not None:
                    print(f"  {b}: {r['trials']} trials, {r['kept']} kept, {r['moved']} parts moved, stop: {r['stop']}, {r['place_s']} s"
                          + "".join(f"\n    + {c}" for c in r["changes"]))
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps({"boards": common, "runs": {n: {b: runs[n][b] for b in common} for n in names}}, indent=1) + "\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
