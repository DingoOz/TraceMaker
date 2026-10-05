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
