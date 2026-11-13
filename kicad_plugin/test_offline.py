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

kw, rest = route_kwargs(["--time", "30", "--threads", "4", "--no-kb", "--no-gpu", "--view", "--work", "5000"])
assert kw == {"time_s": 30.0, "threads": 4, "kb": False, "gpu": False, "view": True, "work": 5000} and rest == [], kw
assert route_kwargs([]) == ({}, [])
kw, rest = route_kwargs(["--time", "30", "--power-width", "0.5"])  # options only the binary takes are returned, so the binary is used
assert kw == {"time_s": 30.0} and rest == ["--power-width", "0.5"], (kw, rest)
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

# "Route selected nets" (tracemaker_route_nets.py): the nets of a selection, and a fingerprint that ignores item order but not content.
from tracemaker_route_nets import fingerprint, selected_nets  # noqa: E402


class SelBoard:
    def __init__(self, sel, tracks=(), vias=(), fps=(), zones=()):
        self.sel, self.t, self.v, self.f, self.z = sel, list(tracks), list(vias), list(fps), list(zones)

    def get_selection(self):
        return self.sel

    def get_tracks(self):
        return self.t

    def get_vias(self):
        return self.v

    def get_footprints(self):
        return self.f

    def get_zones(self):
        return self.z


pad_a = NS(net=NS(name="A"))
fp = NS(net=None, definition=NS(pads=[NS(net=NS(name="C")), NS(net=NS(name="")), NS(net=NS(name="B"))]))
assert selected_nets(SelBoard([pad_a, fp, NS(net=NS(name="A"))])) == ["A", "B", "C"]
assert selected_nets(SelBoard([NS()])) == []
t1, t2 = Track(start=pt(0, 0), end=pt(1, 0)), Track(start=pt(0, 0), end=pt(2, 0))
f12 = fingerprint(SelBoard([], tracks=[t1, t2]))
assert f12 == fingerprint(SelBoard([], tracks=[t2, t1]))
assert f12 != fingerprint(SelBoard([], tracks=[t1]))
assert f12 != fingerprint(SelBoard([], vias=[t1, t2]))  # a via is not a track
print("OK: selected nets and board fingerprint")

# Per-project settings (tracemaker_settings.py, D185): defaults, the project's tracemaker.json, then the environment; read strictly.
import os  # noqa: E402
import tempfile  # noqa: E402

import tracemaker_settings as ts  # noqa: E402

with tempfile.TemporaryDirectory() as d:
    s0, src = ts.load(d, {})
    assert s0 == ts.DEFAULTS and src == "defaults"
    assert ts.route_args(s0) == ["--time-total", "120", "--power-width", "0.5", "--octilinear"]
    with open(os.path.join(d, "tracemaker.json"), "w") as f:
        json.dump({"version": 1, "route_args": "--time 30 --threads 4", "power_width_mm": 0, "octilinear": False, "refill": True, "report": "off"}, f)
    s1, src = ts.load(d, {})
    assert s1["refill"] is True and s1["report"] == "off" and src.endswith("tracemaker.json")
    assert ts.route_args(s1) == ["--time", "30", "--threads", "4"]  # power width 0 and octilinear false: neither option
    s2, src = ts.load(d, {"TRACEMAKER_REFILL": "0", "TRACEMAKER_POWER_WIDTH": "0.8", "TRACEMAKER_REROUTE": "1"})
    assert s2["refill"] is False and s2["power_width_mm"] == 0.8 and s2["reroute"] is True and "TRACEMAKER_REFILL" in src
    assert ts.route_args(dict(s2, route_args="--power-width 1 --octilinear")) == ["--power-width", "1", "--octilinear"]  # explicit options are not doubled
    for bad, word in (({"power_width_mm": "wide"}, "power_width_mm"), ({"refill": "yes"}, "refill"), ({"plaec": "auto"}, "plaec"),
                      ({"place": "anywhere"}, "place"), ({"net_work": 0}, "net_work"), ({"version": 2}, "version")):
        with open(os.path.join(d, "tracemaker.json"), "w") as f:
            json.dump(bad, f)
        try:
            ts.load(d, {})
            raise AssertionError(f"accepted {bad}")
        except ts.SettingsError as e:
            assert word in str(e), (bad, str(e))
    with open(os.path.join(d, "tracemaker.json"), "w") as f:
        f.write("{not json")
    try:
        ts.load(d, {})
        raise AssertionError("accepted bad JSON")
    except ts.SettingsError as e:
        assert "not valid JSON" in str(e)
assert ts.load(None, {})[0] == ts.DEFAULTS  # no project: defaults
print("OK: per-project settings")

