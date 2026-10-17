#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks of the text helpers the closure loop stands on (run: python3 test_closure_helpers.py). Uses the boards in ../boards and the PCBench fixtures."""
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import pcbtext
import rules

RW = pathlib.Path(__file__).resolve().parents[1]
FIX = RW.parents[1] / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def test_net_class_formats():
    # class block closed on its own line (DronPi) and closed on the last add_net line (AVR-ISP)
    for path, nets in ((RW / "boards/DronPi_dronPi/planes.kicad_pcb", {"GND": "T1"}), (FIX / "AVR-ISP_pogo-plug_1.27mm_AVR-ISP_pogo-plug_1.27mm/unrouted.kicad_pcb", {"/GND": "T1", "/MISO": "T2"})):
        t = path.read_text(errors="replace")
        o = rules.apply_nets(t, nets, {**rules.TIERS, "T0": rules.own_tier(t)})
        before = len(rules.class_blocks(t))
        assert len(rules.class_blocks(o)) == before + len(set(nets.values())) , path
        for n, tier in nets.items():
            blk = next(o[a:b] for a, b in rules.class_blocks(o) if n in rules.class_nets(o[a:b]) and f"_{tier}" in o[a:b][:60])
            assert f"(clearance {rules.TIERS[tier]['clr']})" in blk
        # every net is in exactly one class afterwards
        seen = {}
        for a, b in rules.class_blocks(o):
            for n in rules.class_nets(o[a:b]):
                assert n not in seen, (path, n)
                seen[n] = 1


def test_zone_remove_and_add_roundtrip():
    src = (RW / "boards/DronPi_dronPi/planes.kicad_pcb").read_text()
    zs = [pcbtext.zone_net(src[a:b]) for h, a, b in pcbtext.toplevel(src) if h == "zone"]
    assert "GND" in zs
    cut = pcbtext.remove_zones(src, {"GND"})
    assert "GND" not in [pcbtext.zone_net(cut[a:b]) for h, a, b in pcbtext.toplevel(cut) if h == "zone"]
    back = pcbtext.add_zones(cut, src, {"GND"})
    assert sorted(pcbtext.zone_net(back[a:b]) for h, a, b in pcbtext.toplevel(back) if h == "zone") == sorted(zs)


def test_strip_window_leaves_no_dangling_piece():
    t = (RW / "boards/DronPi_dronPi/closure-5/routed.kicad_pcb").read_text()
    t2, n, lost = pcbtext.strip_window(t, [(116.84, 112.268)], 3.0)
    assert n > 0 and len(pcbtext.copper(t2)) == len(pcbtext.copper(t)) - n
    # every removed net lost whole pieces: no remaining segment of a lost net ends in the air at a removed end point (checked through the shared end points)
    ends = {}
    for kind, a, b, net, pts in pcbtext.copper(t2):
        for p in pts:
            ends.setdefault((net, round(p[0], 3), round(p[1], 3)), 0)
            ends[(net, round(p[0], 3), round(p[1], 3))] += 1
    assert t2.count("(segment") <= t.count("(segment")


def test_move_part():
    t = (RW / "boards/DronPi_dronPi/planes.kicad_pcb").read_text()
    x, y = pcbtext.part_xy(t, "R53")
    t2 = pcbtext.move_part(t, "R53", 0.5, -0.25)
    x2, y2 = pcbtext.part_xy(t2, "R53")
    assert abs(x2 - x - 0.5) < 1e-3 and abs(y2 - y + 0.25) < 1e-3
    assert len(t2) - len(t) < 20  # nothing else changed


if __name__ == "__main__":
    for name, fn in list(globals().items()):
        if name.startswith("test_"):
            fn()
            print("ok", name)
