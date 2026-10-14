#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The reference-return audit through the command line (issue #7, doc 05 section 42):

  return_paths.py <tracemaker> <workdir>

- `route --return-paths` is read-only: the routed board is byte-identical with and without it, and the report is in the summary
- `return-paths` on a human-routed demo board (skipped if the fixture is missing): a report per net, JSON, --strict exits 4 on a warning,
  no selector is an error, an unmatched pattern is not, and the report is the same twice
"""
import hashlib
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEMO = ROOT / "bench/data/kicad/demos/royalblue54L_feather/RoyalBlue54L-Feather.kicad_pcb"


def sha(p):
    return hashlib.sha256(pathlib.Path(p).read_bytes()).hexdigest()


def problem():
    obstacles, connections = [], []
    for i, part, x, y in [("a1", "U1", -8, 0), ("a2", "U2", 8, 0), ("b1", "U3", 0, -6), ("b2", "U4", 0, 6)]:
        obstacles.append({"type": "rect", "layers": ["top"], "center": {"x": x, "y": y}, "width": 0.6, "height": 0.5, "connectedTo": [i], "obstacleId": "pad_" + i, "componentId": part})
    connections.append({"name": "DDR_A", "pointsToConnect": [{"x": -8, "y": 0, "layer": "top", "pointId": "a1"}, {"x": 8, "y": 0, "layer": "top", "pointId": "a2"}], "rootConnectionName": "DDR_A"})
    connections.append({"name": "DDR_B", "pointsToConnect": [{"x": 0, "y": -6, "layer": "top", "pointId": "b1"}, {"x": 0, "y": 6, "layer": "top", "pointId": "b2"}], "rootConnectionName": "DDR_B"})
    return {"layerCount": 2, "minTraceWidth": 0.12, "bounds": {"minX": -10, "maxX": 10, "minY": -10, "maxY": 10}, "obstacles": obstacles, "connections": connections}


def main():
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    (work / "p.json").write_text(json.dumps(problem()))
    board = work / "b.kicad_pcb"
    subprocess.run([tm, "srj-import", str(work / "p.json"), "-o", str(board)], check=True, capture_output=True)
    base = ["--work", "300000", "--variants", "2", "--threads", "2", "--no-kb"]
    subprocess.run([tm, "route", str(board), "-o", str(work / "plain.kicad_pcb"), "--json", str(work / "plain.json")] + base, capture_output=True, text=True)
    p = subprocess.run([tm, "route", str(board), "-o", str(work / "rp.kicad_pcb"), "--json", str(work / "rp.json"), "--return-paths", "DDR*"] + base, capture_output=True, text=True)
    check(p.returncode in (0, 3), f"route --return-paths: exit {p.returncode}: {p.stderr[-300:]}")
    check(sha(work / "plain.kicad_pcb") == sha(work / "rp.kicad_pcb"), "--return-paths changed the routed board: the audit must be read-only")
    j = json.loads((work / "rp.json").read_text())
    rp = j.get("return_paths")
    check(rp is not None and len(rp["nets"]) == 2, f"the summary should have the audit of both nets: {rp and len(rp['nets'])}")
    if rp:
        check(all(n["layers"][0]["reference"] == "" or n["layers"][0]["reference"] is None for n in rp["nets"]), "a board without planes has no reference layer")
        check(any("no copper zones" in note for note in rp["notes"]), f"the notes should say there are no zones: {rp['notes']}")
        check(all(any("no reference layer" in w for w in n["warnings"]) for n in rp["nets"]), "each net should warn that it has no reference")
    check("return_paths" not in json.loads((work / "plain.json").read_text()), "no audit unless asked")

    if not DEMO.exists():
        print("demo fixture missing: the board part is skipped")
        print("return_paths: " + ("ok" if ok else "FAILED"))
        return 0 if ok else 1
    args = [tm, "return-paths", str(DEMO), "--nets", "Net-(J8-SW*"]
    a = subprocess.run(args + ["--json", str(work / "a.json")], capture_output=True, text=True)
    check(a.returncode == 0, f"return-paths: exit {a.returncode}: {a.stderr[-300:]}")
    check("Net-(J8-SWDIO)" in a.stdout and "F.Cu over In1.Cu" in a.stdout, f"the text report should name the nets and their layers: {a.stdout[:300]}")
    ja = json.loads((work / "a.json").read_text())
    check(len(ja["nets"]) == 2 and all(n["signal_mm"] > 0 and "voids" in n for n in ja["nets"]), "the JSON should have both nets with lengths")
    check(any(n["detour_mm"] > 0 for n in ja["nets"]), "this board's SWD nets have antipads and layer changes: a detour was expected")
    b = subprocess.run(args + ["--json", str(work / "b.json")], capture_output=True, text=True)
    check((work / "a.json").read_text() == (work / "b.json").read_text(), "the same board and options gave a different report")
    s = subprocess.run(args + ["--strict", "--warn-detour-mm", "1.0"], capture_output=True, text=True)
    check(s.returncode == 4, f"--strict with a detour over the threshold should exit 4, got {s.returncode}")
    # The detours here are 1.4 to 2.1 mm: a threshold of 3 mm is clear of the tie in the choice of where the return regains copper (doc 05 section 42, limits)
    q = subprocess.run(args + ["--strict", "--warn-detour-mm", "3.0"], capture_output=True, text=True)
    check(q.returncode == 0, f"--strict with nothing over the thresholds should exit 0, got {q.returncode}: {q.stdout[-300:]}")
    n = subprocess.run([tm, "return-paths", str(DEMO)], capture_output=True, text=True)
    check(n.returncode == 1 and "no nets selected" in n.stderr, f"no selector should be an error: {n.returncode} {n.stderr[-200:]}")
    u = subprocess.run([tm, "return-paths", str(DEMO), "--nets", "NO_SUCH_NET_*", "--strict"], capture_output=True, text=True)
    check(u.returncode == 0 and "no nets matched" in u.stdout, f"an unmatched pattern is not an error: {u.returncode} {u.stdout[-100:]}")
    r = subprocess.run(args + ["--reference-layer", "Net-(J8-SWDIO)=In2.Cu", "--json", str(work / "c.json")], capture_output=True, text=True)
    jc = json.loads((work / "c.json").read_text()) if (work / "c.json").exists() else {"nets": []}
    forced = [n for n in jc["nets"] if n["net"] == "Net-(J8-SWDIO)"]
    check(r.returncode == 0 and forced and forced[0]["layers"][0]["reference"] == "In2.Cu", f"--reference-layer should force the reference: {forced and forced[0]['layers']}")
    bad = subprocess.run(args + ["--reference-layer", "oops"], capture_output=True, text=True)
    check(bad.returncode == 1 and "PATTERN=LAYER" in bad.stderr, "a bad --reference-layer is an error naming the format")
    print("return_paths: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
