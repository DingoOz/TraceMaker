#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Simple Route JSON end to end (doc 08 §9) on tests/srj/tiny.json: srj-import -> route -> srj-export.

  srj_roundtrip.py <tracemaker> <work dir>

The problem has three routable connections (one needs a via) and one whose pad is walled in on both layers.
Checks:
  * the conversion is byte-identical when repeated, and the router counts as many connections as the problem has;
  * routing at a fixed work budget connects the three and reports the fourth unrouted (exit code 3);
  * the exported traces name exactly the routed connections; every wire point lies on a routed track of that net
    and layer or inside a pad of the net; every via is a via of the board; each trace starts and ends at the
    problem's own points; the export reports the walled-in connection as unrouted;
  * the export is byte-identical when repeated and keeps the problem's other fields;
  * when kicad-cli is available, KiCad counts the same unconnected items before and after and adds no error.
"""
import json
import math
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "tests/srj/tiny.json"
NM = 1e6
LAYER = {"top": "F.Cu", "bottom": "B.Cu"}


def run(cmd, ok=(0,)):
    r = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if r.returncode not in ok:
        sys.exit(f"{' '.join(str(c) for c in cmd)}\nexit {r.returncode}\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    return r


def seg_dist(px, py, t):
    ax, ay, bx, by = t["sx"], t["sy"], t["ex"], t["ey"]
    dx, dy = bx - ax, by - ay
    u = 0.0 if dx == dy == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / (dx * dx + dy * dy)))
    return math.hypot(px - ax - u * dx, py - ay - u * dy)


def in_pad(px, py, p):
    a = math.radians(p["angle"])  # KiCad angle: counter-clockwise on screen, y down
    dx, dy = px - p["x"], py - p["y"]
    lx, ly = dx * math.cos(a) - dy * math.sin(a), dx * math.sin(a) + dy * math.cos(a)
    return abs(lx) <= p["w"] / 2 + 2 and abs(ly) <= p["h"] / 2 + 2


def kicad_drc(board):
    out = board.with_suffix(".drc.json")
    subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "-o", str(out), str(board)],
                   capture_output=True, timeout=600)
    return json.loads(out.read_text()) if out.exists() else None


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    errors = []
    problem = json.loads(SRC.read_text())

    board, again = work / "tiny.kicad_pcb", work / "again" / "tiny.kicad_pcb"
    again.parent.mkdir(exist_ok=True)
    run([tm, "srj-import", SRC, "-o", board, "--json", work / "import.json"])
    run([tm, "srj-import", SRC, "-o", again])
    for ext in (".kicad_pcb", ".kicad_pro"):
        if board.with_suffix(ext).read_bytes() != again.with_suffix(ext).read_bytes():
            errors.append(f"srj-import is not deterministic: {ext} differs between two runs")
    imp = json.loads((work / "import.json").read_text())
    if imp["srj_pairs"] != len(problem["connections"]) or imp["board_pairs"] != imp["srj_pairs"]:
        errors.append(f"connection count: problem {len(problem['connections'])}, import {imp['srj_pairs']}, board {imp['board_pairs']}")

    routed = work / "routed.kicad_pcb"
    shutil.copy(board.with_suffix(".kicad_pro"), routed.with_suffix(".kicad_pro"))
    r = run([tm, "route", board, "-o", routed, "--work", "2000000", "--time", "3600", "--variants", "1", "--threads", "1", "--no-kb", "--no-gpu",
             "--json", work / "route.json"], ok=(0, 3))
    res = json.loads((work / "route.json").read_text())
    if res["connections"] != len(problem["connections"]):
        errors.append(f"route counts {res['connections']} connections, the problem has {len(problem['connections'])}")
    if res["routed"] != 3 or r.returncode != 3:
        errors.append(f"expected 3 of 4 routed and exit code 3, got {res['routed']}/{res['connections']} and {r.returncode}")

    solved = work / "solved.json"
    e = run([tm, "srj-export", routed, "--problem", SRC, "-o", solved, "--json", work / "export.json"], ok=(0, 3))
    first = solved.read_bytes()
    run([tm, "srj-export", routed, "--problem", SRC, "-o", solved], ok=(0, 3))
    if solved.read_bytes() != first:
        errors.append("srj-export is not deterministic")
    exp = json.loads((work / "export.json").read_text())
    if e.returncode != 3 or exp["unrouted"] != ["d"] or exp["routed"] != 3:
        errors.append(f"export: expected d unrouted and exit code 3, got {exp['unrouted']}, {exp['routed']} routed, exit {e.returncode}")
    out = json.loads(solved.read_text())
    if {k: v for k, v in out.items() if k != "traces"} != problem:
        errors.append("the export changed the problem's own fields")

    truth = work / "routed.truth.json"
    run([tm, "inspect", routed, "--json", truth])
    b = json.loads(truth.read_text())
    named = {t["connection_name"] for t in out["traces"]}
    if named != {"a", "b", "c"}:
        errors.append(f"traces name {sorted(named)}, expected a, b, c")
    points = {c["name"]: c["pointsToConnect"] for c in problem["connections"]}
    vias_seen = 0
    for t in out["traces"]:
        net = t["connection_name"]
        wires = [p for p in t["route"] if p["route_type"] == "wire"]
        for p in wires:
            x, y = p["x"] * NM, -p["y"] * NM
            on_track = any(k["net"] == net and k["layer"] == LAYER[p["layer"]] and seg_dist(x, y, k) <= 2 for k in b["tracks"])
            on_pad = any(k["net"] == net and LAYER[p["layer"]] in k["layers"] and in_pad(x, y, k) for k in b["pads"])
            if not (on_track or on_pad):
                errors.append(f"trace {t['pcb_trace_id']}: point ({p['x']}, {p['y']}) on {p['layer']} is not on routed copper")
        for p in t["route"]:
            if p["route_type"] == "via":
                vias_seen += 1
                if not any(v["net"] == net and abs(v["x"] - p["x"] * NM) <= 2 and abs(v["y"] + p["y"] * NM) <= 2 for v in b["vias"]):
                    errors.append(f"trace {t['pcb_trace_id']}: via at ({p['x']}, {p['y']}) is not on the board")
        if "extra" not in t["pcb_trace_id"]:
            ends = [(wires[0]["x"], wires[0]["y"], wires[0]["layer"]), (wires[-1]["x"], wires[-1]["y"], wires[-1]["layer"])]
            want = [(q["x"], q["y"], q["layer"]) for q in points[net]]
            if ends != want:
                errors.append(f"trace {t['pcb_trace_id']} runs {ends}, the connection is {want}")
    if vias_seen != len(b["vias"]) or exp["vias"] != len(b["vias"]):
        errors.append(f"{len(b['vias'])} vias on the board, {vias_seen} in the traces, {exp['vias']} reported")
    if not b["vias"]:
        errors.append("connection b changes layer: expected a via")

    if shutil.which("kicad-cli"):
        before, after = kicad_drc(board), kicad_drc(routed)
        if before is None or after is None:
            errors.append("kicad-cli did not judge the boards")
        else:
            if len(before["unconnected_items"]) != len(problem["connections"]):
                errors.append(f"KiCad's ratsnest has {len(before['unconnected_items'])} items, the problem {len(problem['connections'])} connections")
            if len(after["unconnected_items"]) != 1:
                errors.append(f"KiCad finds {len(after['unconnected_items'])} unconnected items after routing, expected 1")
            n0 = sum(v["severity"] == "error" for v in before["violations"])
            n1 = sum(v["severity"] == "error" for v in after["violations"])
            if n1 > n0:
                errors.append(f"KiCad DRC errors went from {n0} to {n1}")
    else:
        print("kicad-cli not found: KiCad's verdict not checked")

    for m in errors:
        print("FAIL:", m)
    print(f"srj round trip: {res['routed']}/{res['connections']} routed, {len(out['traces'])} traces, {len(b['vias'])} vias, {len(errors)} errors")
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
