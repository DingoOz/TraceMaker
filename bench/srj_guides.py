#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Turn solved Simple Route JSON traces into a `tracemaker route --guide` file (doc 16 E1, the topology oracle).

  bench/srj_guides.py PROBLEM.json SOLVED.json OUT.json

PROBLEM.json is the SRJ problem (connections with their two points), SOLVED.json holds {"traces": [...]} as written
by a router (tscircuit's pipeline, or `tracemaker srj-export`). One guide per connection: its two end points, the
wire segments with their layer index (top = 0, bottom = layerCount - 1, innerN = N) and the via positions.
"""
import json, sys

def main():
    prob = json.load(open(sys.argv[1]))
    solved = json.load(open(sys.argv[2]))
    nl = prob.get("layerCount", 2)
    def z(name):
        if name == "top": return 0
        if name == "bottom": return nl - 1
        return int(name[5:])
    ends = {}
    for c in prob["connections"]:
        pts = c["pointsToConnect"]
        ends[c["name"]] = ((pts[0]["x"], pts[0]["y"]), (pts[-1]["x"], pts[-1]["y"]))
    by = {}
    for t in solved["traces"]:
        name = t.get("connection_name") or t.get("connectionName")
        if name not in ends: continue
        segs, vias, prev = [], [], None
        for p in t["route"]:
            if p["route_type"] == "via":
                vias.append([p["x"], p["y"]]); prev = None
                continue
            if prev is not None and prev["layer"] == p["layer"]:
                segs.append([prev["x"], prev["y"], p["x"], p["y"], z(p["layer"])])
            prev = p
        g = by.setdefault(name, {"a": list(ends[name][0]), "b": list(ends[name][1]), "segs": [], "vias": []})
        g["segs"] += segs
        g["vias"] += vias
    out = list(by.values())
    json.dump(out, open(sys.argv[3], "w"))

main()
