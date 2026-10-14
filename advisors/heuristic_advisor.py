#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A non-LLM advisor (doc 17 §7): reads a stuck report on stdin, writes an advice file on stdout.

It exists to prove the hint channel without a model (issue #26, stage 2) and as the smallest example of the advisor protocol:
standard library only, no network. The strategies are plain rules over the report:

  stuck-first   the nets of the open connections are routed first (and other nets cannot rip them)
  corridor      a strip along the straight line of each open connection is reserved for its net on the layer its start pad is on
  mixed         both

  --layer-cost LAYER=FACTOR and --priority-pairs N add the other two hint types to whichever strategy runs.

  heuristic_advisor.py [--strategy mixed] [--half-width 0.35] [--max-regions 12] [--keep-vias]
"""
import argparse
import json
import math
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--strategy", choices=["stuck-first", "corridor", "mixed"], default="mixed")
    ap.add_argument("--half-width", type=float, default=0.35, help="half width of a reserved strip, mm")
    ap.add_argument("--end-gap", type=float, default=0.6, help="how far a strip stops short of each pad, mm")
    ap.add_argument("--max-regions", type=int, default=12)
    ap.add_argument("--max-nets", type=int, default=24)
    ap.add_argument("--keep-vias", action="store_true", help="a strip keeps other nets' tracks out but lets their vias through")
    ap.add_argument("--variants", type=int, default=4)
    ap.add_argument("--layer-cost", action="append", default=[], metavar="LAYER=FACTOR", help="a track cost factor for a layer (repeatable)")
    ap.add_argument("--priority-pairs", type=int, default=0, help="also list this many shortest open connections as priority pairs")
    a = ap.parse_args()
    rep = json.load(sys.stdin)
    if rep.get("tracemaker_stuck_report") != 1:
        print("not a stuck report", file=sys.stderr)
        return 2
    unrouted = rep.get("unrouted", [])
    advice = {"version": 1, "variants": a.variants, "note": f"heuristic advisor, strategy {a.strategy}"}
    nets = []
    for u in unrouted:
        if u["net"] not in nets:
            nets.append(u["net"])
    if a.strategy in ("stuck-first", "mixed") and nets:
        advice["first_nets"] = nets[: a.max_nets]
    if a.strategy in ("corridor", "mixed"):
        regions = []
        # Shortest first: they are the likeliest to be walled in by a few neighbours, and a long strip blocks the most.
        for u in sorted(unrouted, key=lambda u: u["length_mm"]):
            if "zone" in u["to"] or len(regions) >= a.max_regions:
                continue
            x0, y0, x1, y1 = u["from"]["x_mm"], u["from"]["y_mm"], u["to"]["x_mm"], u["to"]["y_mm"]
            d = math.hypot(x1 - x0, y1 - y0)
            if d < 2 * a.end_gap + 0.5:
                continue
            ux, uy = (x1 - x0) / d, (y1 - y0) / d
            sx, sy, ex, ey = x0 + ux * a.end_gap, y0 + uy * a.end_gap, x1 - ux * a.end_gap, y1 - uy * a.end_gap
            nx, ny = -uy * a.half_width, ux * a.half_width
            poly = [[round(sx + nx, 3), round(sy + ny, 3)], [round(ex + nx, 3), round(ey + ny, 3)],
                    [round(ex - nx, 3), round(ey - ny, 3)], [round(sx - nx, 3), round(sy - ny, 3)]]
            r = {"name": f"strip {u['net']}", "polygon": poly, "reserve_for": [u["net"]], "layers": u["from"]["layers"]}
            if a.keep_vias:
                r["vias"] = False
            regions.append(r)
        if regions:
            advice["regions"] = regions
    if a.layer_cost:
        advice["layers"] = {"cost": {kv.split("=")[0]: float(kv.split("=")[1]) for kv in a.layer_cost}}
    if a.priority_pairs:
        pairs = [[f"{u['from']['part']}.{u['from']['pad']}", f"{u['to']['part']}.{u['to']['pad']}"]
                 for u in sorted(unrouted, key=lambda u: u["length_mm"]) if "zone" not in u["to"]]
        advice["priority"] = pairs[: a.priority_pairs]
    json.dump(advice, sys.stdout)
    return 0


if __name__ == "__main__":
    sys.exit(main())
