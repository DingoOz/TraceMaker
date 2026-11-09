# SPDX-License-Identifier: GPL-3.0-or-later
"""Offline check: TraceMaker's --emit-items JSON becomes valid kipy Track/Via objects (no KiCad needed)."""
import json
import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])
from tracemaker_route import build_items  # noqa: E402


class FakeNet:
    def __init__(self, name):
        self.name = name


class FakeBoard:
    """Just the lookups build_items uses."""

    def get_nets(self):
        return []  # kipy Net objects need a live board; net assignment is skipped offline

    def get_layer_by_name(self, name):
        from kipy.proto.board import board_types_pb2 as p
        return {"F.Cu": p.BL_F_Cu, "B.Cu": p.BL_B_Cu}.get(name, p.BL_F_Cu)


items = json.load(open(sys.argv[1]))
objs = build_items(FakeBoard(), items)
tracks = [o for o in objs if type(o).__name__ == "Track"]
vias = [o for o in objs if type(o).__name__ == "Via"]
assert len(tracks) == len(items["tracks"]) and len(vias) == len(items["vias"])
t0 = items["tracks"][0]
assert tracks[0].start.x == t0["start"][0] and tracks[0].width == t0["width"]
print(f"OK: {len(tracks)} tracks, {len(vias)} vias built")

# TRACEMAKER_ARGS (CLI spelling) maps onto the Python module's keyword arguments.
from tracemaker_route import route_kwargs  # noqa: E402

kw = route_kwargs(["--time", "30", "--threads", "4", "--no-kb", "--no-gpu", "--view", "--work", "5000"])
assert kw == {"time_s": 30.0, "threads": 4, "kb": False, "gpu": False, "view": True, "work": 5000}, kw
assert route_kwargs([]) == {}
print("OK: TRACEMAKER_ARGS mapping")

# The repair's lifted copper ("removed" in --emit-items, doc 05 §73) is found on the open board by geometry, layer and net.
from types import SimpleNamespace as NS  # noqa: E402

from tracemaker_route import removed_items  # noqa: E402


class Track(NS):
    pass


class LiftBoard(FakeBoard):
    def __init__(self, tracks, vias):
        self.tracks, self.vias = tracks, vias

    def get_tracks(self):
        return self.tracks

    def get_vias(self):
        return self.vias


def pt(x, y):
    return NS(x=x, y=y)


fcu = FakeBoard().get_layer_by_name("F.Cu")
keep = Track(start=pt(0, 0), end=pt(5, 0), layer=fcu, net=NS(name="A"), locked=False)
lift = Track(start=pt(1, 1), end=pt(2, 2), layer=fcu, net=NS(name="B"), locked=False)
locked = Track(start=pt(1, 1), end=pt(2, 2), layer=fcu, net=NS(name="B"), locked=True)
via = NS(position=pt(3, 3), net=NS(name="B"), locked=False)
board = LiftBoard([keep, locked, lift], [via])
gone = {"removed": {"tracks": [{"start": [2, 2], "end": [1, 1], "layer": "F.Cu", "net": "B"}], "vias": [{"position": [3, 3], "net": "B"}]}}
found, missing = removed_items(board, gone)
assert found == [lift, via] and missing == 0, (found, missing)
found, missing = removed_items(board, {"removed": {"tracks": [{"start": [9, 9], "end": [1, 1], "layer": "F.Cu", "net": "B"}]}})
assert found == [] and missing == 1
assert removed_items(board, {"tracks": []}) == ([], 0)
print("OK: lifted copper matched on the open board")
