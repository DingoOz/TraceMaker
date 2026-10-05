"""KiCad 10 IPC action plugin: autoroute the open board with TraceMaker.

Flow: save a copy of the open board (with its project, so all design rules are available), route the copy, then
add the new tracks and vias to the open board in ONE commit, so a single undo removes the whole result. The open
board itself is never overwritten.

The engine runs in-process through the Python module `tracemaker` (bindings/) when it can be imported, and
otherwise as the `tracemaker` binary (`tracemaker route ... --emit-items`). Both run the same code.

Environment:
  TRACEMAKER                path to the tracemaker binary (default: `bin/tracemaker` next to this file, then PATH)
  TRACEMAKER_PYTHONPATH     directory holding the tracemaker Python module (default: `lib/` next to this file)
  TRACEMAKER_ARGS           route options, e.g. "--time 120 --threads 8 --view" (CLI spelling for both paths)
  TRACEMAKER_NO_BINDINGS=1  always use the binary
"""
from __future__ import annotations

import argparse
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

HERE = os.path.dirname(os.path.abspath(__file__))


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


def find_bindings():
    """The `tracemaker` Python module, or None (then the binary is used)."""
    if os.environ.get("TRACEMAKER_NO_BINDINGS"):
        return None
    for d in (os.environ.get("TRACEMAKER_PYTHONPATH"), os.path.join(HERE, "lib")):
        if d and os.path.isdir(d) and d not in sys.path:
            sys.path.insert(0, d)
    try:
        import tracemaker
    except ImportError:
        return None
    return tracemaker if hasattr(tracemaker, "route") else None


def find_binary():
    exe = os.environ.get("TRACEMAKER")
    if exe:
        return exe
    bundled = os.path.join(HERE, "bin", "tracemaker")
    if os.path.isfile(bundled):
        if not os.access(bundled, os.X_OK):  # the package manager may not keep the executable bit
            try:
                os.chmod(bundled, 0o755)
            except OSError:
                pass
        if os.access(bundled, os.X_OK):
            return bundled
    return shutil.which("tracemaker")


def route_kwargs(args: list) -> dict:
    """Maps the CLI's route options (TRACEMAKER_ARGS) to tracemaker.route() keyword arguments."""
    p = argparse.ArgumentParser(add_help=False)
    p.add_argument("--time", dest="time_s", type=float)
    p.add_argument("--threads", type=int)
    p.add_argument("--variants", type=int)
    p.add_argument("--work", type=int)
    p.add_argument("--seed", type=int)
    p.add_argument("--pitch-um", dest="pitch_um", type=float)
    p.add_argument("--via-cost-mm", dest="via_cost_mm", type=float)
    p.add_argument("--view", action="store_true", default=None)
    p.add_argument("--view-host")
    p.add_argument("--view-port", type=int)
    p.add_argument("--kb")
    p.add_argument("--no-kb", dest="kb", action="store_false")
    p.add_argument("--no-gpu", dest="gpu", action="store_false", default=None)
    p.add_argument("--no-optimize", dest="optimize", action="store_false", default=None)
    p.add_argument("--no-rip-up", dest="rip_up", action="store_false", default=None)
    p.add_argument("--global", dest="global_route", action="store_true", default=None)
    p.add_argument("--record")
    known, unknown = p.parse_known_args(args)
    if unknown:
        print(f"TraceMaker: ignoring options the Python module does not take: {' '.join(unknown)}", file=sys.stderr)
    return {k: v for k, v in vars(known).items() if v is not None}


def run_engine(src: str, out: str, items_path: str) -> bool:
    """Routes `src`, writing the routed board to `out` and the new copper as JSON to `items_path`."""
    args = shlex.split(os.environ.get("TRACEMAKER_ARGS", "--time 120"))
    tm = find_bindings()
    if tm is not None:
        print(f"TraceMaker {tm.__version__} (Python module)", flush=True)
        try:
            r = tm.route(src, out, items_out=items_path, verbose=True, **route_kwargs(args))
        except Exception as e:  # noqa: BLE001 - shown to the user in KiCad's plugin output
            print(f"TraceMaker: {e}", file=sys.stderr)
            return False
        return r["exit_code"] in (0, 3) and os.path.exists(items_path)
    exe = find_binary()
    if not exe:
        print("TraceMaker: set TRACEMAKER to the tracemaker binary (or TRACEMAKER_PYTHONPATH to the module)", file=sys.stderr)
        return False
    proc = subprocess.run([exe, "route", src, "-o", out, "--emit-items", items_path] + args, capture_output=True, text=True)
    print(proc.stdout)
    if proc.returncode not in (0, 3) or not os.path.exists(items_path):
        print(proc.stderr, file=sys.stderr)
        return False
    return True


def main() -> int:
    kicad = KiCad()
    board = kicad.get_board()
    with tempfile.TemporaryDirectory(prefix="tracemaker-") as tmp:
        src = os.path.join(tmp, "board.kicad_pcb")
        board.save_as(src, overwrite=True, include_project=True)
        items_path = os.path.join(tmp, "items.json")
        if not run_engine(src, os.path.join(tmp, "routed.kicad_pcb"), items_path):
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
