#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Issue #55: one line per bench/run.py run with --place, all boards together: every decoupling cap (two-pad C* between a
power-like net and ground that reaches an IC pad), its distance to the nearest IC pad on its net (median, share within
DEC-01's 5 mm); for each board's largest IC with 4 or more caps, the share of them on its busiest side (mean); routed.

  bench/records/issue55/summarise.py RUN ..."""
import json
import pathlib
import statistics
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(HERE))
import compare_runs as cr  # noqa: E402
import decap_sides as ds  # noqa: E402

def all_decaps(path):
    b = ds.load(path)
    ic_pads = {}
    for p in b.parts:
        if p.ref.startswith(("U", "IC")):
            for q in p.pads:
                if q.net:
                    ic_pads.setdefault(q.net, []).append(q)
    out = []
    for p in b.parts:
        if not p.ref.startswith("C") or len(p.pads) != 2:
            continue
        a, c = p.pads
        sp = a if a.net and ds.is_supply(a.netname) and c.net and ds.is_gnd(c.netname) else \
            c if c.net and ds.is_supply(c.netname) and a.net and ds.is_gnd(a.netname) else None
        if sp is None or sp.net not in ic_pads:
            continue
        out.append(min(((q.x - sp.x) ** 2 + (q.y - sp.y) ** 2) ** 0.5 for q in ic_pads[sp.net]))
    return out


print("| Run | decaps | median mm | within 5 mm | busiest side share (mean) | routed | of |")
print("|---|--:|--:|--:|--:|--:|--:|")
for run in sys.argv[1:]:
    rows = cr.rows(run)
    d, share, routed, conns = [], [], 0, 0
    for name, r in sorted(rows.items()):
        pb = ROOT / "bench/results" / run / "boards" / f"{name}.placed.kicad_pcb"
        if not pb.exists():
            continue
        ic = cr.biggest_ic(str(pb))
        v = ds.analyse(str(pb), [ic])[ic]
        if v["caps"] >= 4:
            share.append(max(v["sides"].values()) / v["caps"])
        d += all_decaps(str(pb))
        routed += r.get("routed") or 0
        conns += r.get("connections") or 0
    within = sum(x <= 5 for x in d)
    print(f"| {run} | {len(d)} | {statistics.median(d):.2f} | {within} ({100 * within / len(d):.0f} %) | {statistics.mean(share):.2f} | {routed} | {conns} |")
