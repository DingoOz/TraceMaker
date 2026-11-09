#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Issue #55: two bench/run.py runs with --place, side by side per board: the decoupling caps round the board's largest IC
(per side, median distance to the nearest supply pin of their net; decap_sides.py), connections routed, clean.

  bench/records/issue55/compare_runs.py RUN_BEFORE RUN_AFTER [--ic BOARD=REF ...]   (runs in bench/results/)"""
import json
import pathlib
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import decap_sides as ds  # noqa: E402


def rows(run):
    out = {}
    for line in (ROOT / "bench/results" / run / "boards.jsonl").read_text().splitlines():
        r = json.loads(line)
        out[r["board"]] = r
    return out


def biggest_ic(path):
    b = ds.load(path)
    ics = [p for p in b.parts if p.ref.startswith(("U", "IC")) and p.ref[len("IC" if p.ref.startswith("IC") else "U"):][:1].isdigit()]
    return max(ics, key=lambda p: (len(p.pads), p.ref)).ref


def fmt(v):
    s = v["sides"]
    return f"{s['left']}/{s['right']}/{s['top']}/{s['bottom']}", v["median_mm"], v["max_mm"]


def main():
    a, b = sys.argv[1], sys.argv[2]
    force = dict(x.split("=", 1) for x in sys.argv[3:] if "=" in x)
    ra, rb = rows(a), rows(b)
    print(f"| Board | IC (pins) | supply pins L/R/T/B | caps L/R/T/B before | after | median mm before | after | max mm before | after "
          f"| routed before | after | clean before | after |")
    print("|---|---|---|---|---|--:|--:|--:|--:|--:|--:|---|---|")
    tot = [0, 0, 0, 0, 0]
    for name in sorted(set(ra) & set(rb)):
        pa = ROOT / "bench/results" / a / "boards" / f"{name}.placed.kicad_pcb"
        pb = ROOT / "bench/results" / b / "boards" / f"{name}.placed.kicad_pcb"
        if not (pa.exists() and pb.exists()):
            continue
        ic = force.get(name) or biggest_ic(str(pa))
        va, vb = ds.analyse(str(pa), [ic])[ic], ds.analyse(str(pb), [ic])[ic]
        npins = len([p for p in ds.load(str(pa)).parts if p.ref == ic][0].pads)
        sp = va["supply_pins"]
        sa, ma, xa = fmt(va)
        sb, mb, xb = fmt(vb)
        x, y = ra[name], rb[name]
        print(f"| {name} | {ic} ({npins}) | {sp['left']}/{sp['right']}/{sp['top']}/{sp['bottom']} | {sa} | {sb} | {ma} | {mb} | {xa} | {xb} | "
              f"{x.get('routed')}/{x.get('connections')} | {y.get('routed')}/{y.get('connections')} | {x.get('clean')} | {y.get('clean')} |")
        tot[0] += x.get("routed") or 0
        tot[1] += y.get("routed") or 0
        tot[2] += x.get("connections") or 0
        tot[3] += bool(x.get("clean"))
        tot[4] += bool(y.get("clean"))
    print(f"\nRouted: {tot[0]} before, {tot[1]} after, of {tot[2]}; clean {tot[3]} before, {tot[4]} after.")


if __name__ == "__main__":
    main()
