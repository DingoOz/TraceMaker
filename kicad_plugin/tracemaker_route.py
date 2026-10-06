# SPDX-License-Identifier: GPL-3.0-or-later
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
  TRACEMAKER_REROUTE=1      remove the unlocked tracks and vias and route the whole board again (same commit)
  TRACEMAKER_PLACE          let TraceMaker move footprints first: refine, auto, routable, eco or full (same commit);
                            needs the tracemaker-place binary (TRACEMAKER_PLACE_BIN, else next to the router binary)
  TRACEMAKER_PLACE_ARGS     extra tracemaker-place options
  TRACEMAKER_REFILL=1       refill the zones after the commit. Off by default, except after footprints moved (=0 stops
                            that too): KiCad records the refill as a step of its own, so the result then takes two
                            undos instead of one
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
from kipy.geometry import Angle, Vector2
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


def find_placer():
    exe = os.environ.get("TRACEMAKER_PLACE_BIN")
    if exe:
        return exe
    router = find_binary()
    cands = [os.path.join(HERE, "bin", "tracemaker-place")]
    if router:
        d = os.path.dirname(os.path.abspath(router))
        cands += [os.path.join(d, "tracemaker-place"), os.path.join(d, "..", "place", "tracemaker-place")]  # install dir, build tree
    for c in cands:
        if os.path.isfile(c) and os.access(c, os.X_OK):
            return c
    return shutil.which("tracemaker-place")


def run_placer(src: str, out: str, report: str, mode: str) -> dict:
    """Places `src` into `out`; returns {reference: (x_nm, y_nm, angle_deg)} for the footprints the placer may move
    (the caller compares with the live board), or {} when placement failed (the board is then routed as it is)."""
    exe = find_placer()
    if not exe:
        print("TraceMaker: TRACEMAKER_PLACE is set but tracemaker-place was not found (set TRACEMAKER_PLACE_BIN)", file=sys.stderr)
        return {}
    proc = subprocess.run([exe, src, "-o", out, "--mode", mode, "--json", report] + shlex.split(os.environ.get("TRACEMAKER_PLACE_ARGS", "")),
                          capture_output=True, text=True)
    if proc.returncode not in (0, 2) or not os.path.exists(out) or not os.path.exists(report):
        print(proc.stdout[-2000:], proc.stderr[-2000:], file=sys.stderr)
        return {}
    with open(report) as f:
        rep = json.load(f)
    seen, moves = {}, {}
    for p in rep.get("placement", []):
        seen[p["ref"]] = seen.get(p["ref"], 0) + 1
    for p in rep.get("placement", []):
        if seen[p["ref"]] != 1 or p.get("flipped") or not p.get("movable"):  # ambiguous, a side change (not applied through the API), or fixed
            continue
        moves[p["ref"]] = (round(p["x"] * 1e6), round(p["y"] * 1e6), float(p["angle"]))
    if rep.get("moved", 0) == 0:
        return {}
    print(f"TraceMaker: placement ({mode}) moved {rep.get('moved')} footprint(s)", flush=True)
    return moves


def run_engine(src: str, out: str, items_path: str, extra: list | None = None) -> bool:
    """Routes `src`, writing the routed board to `out` and the new copper as JSON to `items_path`."""
    args = shlex.split(os.environ.get("TRACEMAKER_ARGS", "--time 120")) + (extra or [])
    tm = None if extra else find_bindings()  # the module takes no --reroute
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
        reroute = os.environ.get("TRACEMAKER_REROUTE") == "1"
        moves = {}
        route_src = src
        if os.environ.get("TRACEMAKER_PLACE"):
            placed = os.path.join(tmp, "board.placed.kicad_pcb")  # same folder: the project files apply to it too
            for ext in (".kicad_pro", ".kicad_dru", ".kicad_prl"):
                if os.path.exists(os.path.join(tmp, "board" + ext)):
                    shutil.copy2(os.path.join(tmp, "board" + ext), os.path.join(tmp, "board.placed" + ext))
            moves = run_placer(src, placed, os.path.join(tmp, "place.json"), os.environ["TRACEMAKER_PLACE"])
            if moves:
                route_src = placed
                # Old tracks were routed to the old positions: with parts moved the board is routed again.
                if not reroute:
                    print("TraceMaker: footprints moved, so the existing unlocked tracks and vias are routed again", flush=True)
                reroute = True
        items_path = os.path.join(tmp, "items.json")
        if not run_engine(route_src, os.path.join(tmp, "routed.kicad_pcb"), items_path, ["--reroute"] if reroute else None):
            return 1
        with open(items_path) as f:
            items = json.load(f)
    new = build_items(board, items)
    if not new and not moves:
        print("TraceMaker: nothing to add")
        return 0
    # One commit: a single undo takes back the removed copper, the moved footprints and the new copper together.
    commit = board.begin_commit()
    removed = 0
    if reroute:
        old = [t for t in board.get_tracks() if type(t).__name__ == "Track" and not t.locked] + [v for v in board.get_vias() if not v.locked]
        if old:
            board.remove_items(old)
        removed = len(old)
    changed = []
    if moves:
        for fp in board.get_footprints():
            m = moves.get(fp.reference_field.text.value)
            if m is None:
                continue
            turn = (fp.orientation.degrees - m[2]) % 360.0
            if (fp.position.x, fp.position.y) == (m[0], m[1]) and min(turn, 360.0 - turn) < 1e-6:
                continue  # where the placer left it
            fp.position = Vector2.from_xy(m[0], m[1])
            fp.orientation = Angle.from_degrees(m[2])
            changed.append(fp)
        if changed:
            board.update_items(changed)
    if new:
        board.create_items(new)
    board.push_commit(commit, "TraceMaker autoroute")
    print(f"TraceMaker: added {len(items['tracks'])} tracks and {len(items['vias'])} vias" + (f", removed {removed} old ones" if reroute else "") +
          (f", moved {len(changed)} footprints" if moves else ""))
    # Moved footprints leave the fills stale (KiCad's DRC then reports clearance errors against them), so they are
    # refilled unless switched off; otherwise only on request.
    refill = os.environ.get("TRACEMAKER_REFILL")
    if (refill == "1" or (changed and refill != "0")) and board.get_zones():
        try:
            board.refill_zones()
            print("TraceMaker: zones refilled")
        except Exception as e:  # noqa: BLE001 - the copper is in; a failed refill is reported, not fatal
            print(f"TraceMaker: zone refill failed: {e}", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
