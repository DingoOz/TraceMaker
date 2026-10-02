"""KiCad 10 IPC action plugin: autoroute the open board with TraceMaker.

Flow: save a copy of the open board (with its project, so all design rules are available), run
`tracemaker route` on the copy, then add the new tracks and vias to the open board in ONE commit, so a single
undo removes the whole result. The open board itself is never overwritten.

Environment: TRACEMAKER (path to the tracemaker binary, default: `tracemaker` on PATH), TRACEMAKER_ARGS (extra
arguments, e.g. "--time 120 --threads 8 --view").
"""
from __future__ import annotations

import json
import os
import shlex
import shutil
import subprocess
import sys
import tempfile

from kipy import KiCad
from kipy.board_types import Track, Via
from kipy.geometry import Vector2
from kipy.proto.board import board_types_pb2


def build_items(board, items: dict) -> list:
    """Turns TraceMaker's --emit-items JSON into kipy Track/Via objects (no KiCad calls besides lookups)."""
    nets = {n.name: n for n in board.get_nets()}
    out = []
    for t in items.get("tracks", []):
        tr = Track()
        tr.start = Vector2.from_xy(int(t["start"][0]), int(t["start"][1]))
        tr.end = Vector2.from_xy(int(t["end"][0]), int(t["end"][1]))
        tr.width = int(t["width"])
        tr.layer = board.get_layer_by_name(t["layer"])
        if t["net"] in nets:
            tr.net = nets[t["net"]]
        out.append(tr)
    for v in items.get("vias", []):
        via = Via()
        via.position = Vector2.from_xy(int(v["position"][0]), int(v["position"][1]))
        via.diameter = int(v["diameter"])
        via.drill_diameter = int(v["drill"])
        via.type = board_types_pb2.VT_THROUGH
        if v["net"] in nets:
            via.net = nets[v["net"]]
        out.append(via)
    return out


def main() -> int:
    exe = os.environ.get("TRACEMAKER") or shutil.which("tracemaker")
    if not exe:
        print("TraceMaker: set TRACEMAKER to the tracemaker binary", file=sys.stderr)
        return 1
    kicad = KiCad()
    board = kicad.get_board()
    with tempfile.TemporaryDirectory(prefix="tracemaker-") as tmp:
        src = os.path.join(tmp, "board.kicad_pcb")
        board.save_as(src, overwrite=True, include_project=True)
        items_path = os.path.join(tmp, "items.json")
        cmd = [exe, "route", src, "-o", os.path.join(tmp, "routed.kicad_pcb"), "--emit-items", items_path]
        cmd += shlex.split(os.environ.get("TRACEMAKER_ARGS", "--time 120"))
        proc = subprocess.run(cmd, capture_output=True, text=True)
        print(proc.stdout)
        if proc.returncode not in (0, 3) or not os.path.exists(items_path):
            print(proc.stderr, file=sys.stderr)
            return 1
        with open(items_path) as f:
            items = json.load(f)
    new = build_items(board, items)
    if not new:
        print("TraceMaker: nothing to add")
        return 0
    commit = board.begin_commit()
    board.create_items(new)
    board.push_commit(commit, "TraceMaker autoroute")
    print(f"TraceMaker: added {len(items['tracks'])} tracks and {len(items['vias'])} vias")
    return 0


if __name__ == "__main__":
    sys.exit(main())
