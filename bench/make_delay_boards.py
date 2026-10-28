#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic boards for time-domain matching (doc 05 §59, doc 18 B2): rules in picoseconds on nets with a tuning profile.

  bench/make_delay_boards.py OUT_DIR [NAME ...]

Writes OUT_DIR/<name>/<name>.kicad_{pcb,pro,dru} and OUT_DIR/manifest.json, with bench/make_bus_boards.py's board
writer (the same call gives the same bytes on any machine).

Cases:
  ddr8   One DDR4-style byte lane: DQ0-7, DM0 and the strobe pair DQS0_P/N, from a controller on the front to a memory
         mounted on the back, four layers, so every lane takes a via. Class BYTE0 has profile DDR: 5.9 ps/mm on the
         outer layers, 6.8 ps/mm inside, 7 ps per mm of via. The rules ask for a delay window on every lane, a skew of
         at most 3 ps across the lane and 0.5 ps within the strobe pair; untuned, the lanes miss all three.
"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from make_bus_boards import Board, project  # noqa: E402

PROFILE = {
    "profile_name": "DDR", "type": 0, "target_impedance": 40.0, "enable_time_domain_tuning": True, "generate_drc_rules": True,
    "via_prop_delay": 7000000, "via_overrides": [],
    "layer_entries": [{"signal_layer": layer, "top_reference_layer": "UNDEFINED", "bottom_reference_layer": "UNDEFINED", "width": 0,
                       "diff_pair_gap": 0, "delay": delay}
                      for layer, delay in (("F.Cu", 5900000), ("In1.Cu", 6800000), ("In2.Cu", 6800000), ("B.Cu", 5900000))]}


def ddr8(name, window=(205, 215)):
    lane = ["/DQ0", "/DQ1", "/DQ2", "/DQ3", "/DQS0_P", "/DQS0_N", "/DQ4", "/DQ5", "/DQ6", "/DQ7", "/DM0"]
    rows = len(lane) + 2
    h = 40.0
    b = Board(name, 48.0, h, layers=4)
    b.dual("U1", 8.0, h / 2 - 4.0, ["/VCC"] + [""] * (rows - 2) + ["/GND"], ["/CK"] + lane + ["/CKE"], pitch=1.0, pad=(1.2, 0.5))
    # The memory sits lower, on the back, at twice the pitch: the lanes fan out with different lengths and each changes layer.
    b.dual("U2", 38.0, h / 2 + 3.0, ["/CK"] + lane + ["/CKE"], ["/VCC"] + [""] * (rows - 2) + ["/GND"], pitch=2.0, pad=(1.2, 0.8), back=True)
    b.hole("H1", 23.0, h / 2 + 1.0, 3.2)
    b.cap("C1", 23.0, 4.0, "/VCC", "/GND")
    byte = {"name": "BYTE0", "clearance": 0.15, "track_width": 0.15, "via_diameter": 0.6, "via_drill": 0.3, "priority": 0, "tuning_profile": "DDR"}
    pro = project(name, [byte], [("/DQ*", "BYTE0"), ("/DM0", "BYTE0")])
    pro["net_settings"]["meta"]["version"] = 5  # older net settings lose their tuning profiles when KiCad migrates them
    pro["tuning_profiles"] = {"meta": {"version": 0}, "tuning_profiles_impedance_geometric": [PROFILE]}
    lo, hi = window
    dru = ("(version 1)\n"
           f"(rule \"byte0_window\"\n\t(condition \"A.hasNetclass('BYTE0')\")\n\t(constraint length (min {lo}ps) (max {hi}ps)))\n"
           "(rule \"byte0_skew\"\n\t(condition \"A.hasNetclass('BYTE0')\")\n\t(constraint skew (max 3ps)))\n"
           "(rule \"dqs_pair\"\n\t(condition \"A.inDiffPair('/DQS0_*')\")\n\t(constraint skew (max 0.5ps) (within_diff_pairs)))\n")
    return b, pro, dru


CASES = {"ddr8": ddr8}


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = pathlib.Path(sys.argv[1])
    manifest = []
    for name in sys.argv[2:] or list(CASES):
        board, pro, dru = CASES[name](name)
        d = out / name
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{name}.kicad_pcb").write_text(board.text())
        (d / f"{name}.kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")
        (d / f"{name}.kicad_dru").write_text(dru)
        manifest.append({"name": name, "board": f"{name}/{name}.kicad_pcb"})
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"{len(manifest)} boards written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
