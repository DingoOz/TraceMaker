#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""`tracemaker route` names the custom rules it cannot apply and obeys the ones it can (doc 05 §27, §34, rule 6).

  route_rule_warnings.py <tracemaker> <work dir>

A small board is routed with four `disallow` rules: one whose condition depends on a rule area and one by net and
layer, which the router applies, and one whose condition does not parse and one that uses a property TraceMaker
does not evaluate. The route log must carry a "warning:" line naming each of the last two and none naming the
first two, and `tracemaker drc` must find no disallowed item in the routed board.
"""
import json
import pathlib
import re
import subprocess
import sys

BOARD = """(kicad_pcb (version 20240108) (generator "pcbnew")
  (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal) (37 "F.SilkS" user)
    (44 "Edge.Cuts" user))
  (net 0 "") (net 1 "SIG") (net 2 "GND")
  (footprint "R" (layer "F.Cu") (at 4 5)
    (property "Reference" "R1" (at 0 0) (layer "F.SilkS"))
    (pad "1" smd rect (at 0 -2) (size 1 1) (layers "F.Cu") (net 1 "SIG"))
    (pad "2" smd rect (at 0 2) (size 1 1) (layers "F.Cu") (net 2 "GND"))
  )
  (footprint "R" (layer "F.Cu") (at 16 5)
    (property "Reference" "R2" (at 0 0) (layer "F.SilkS"))
    (pad "1" smd rect (at 0 -2) (size 1 1) (layers "F.Cu") (net 1 "SIG"))
    (pad "2" smd rect (at 0 2) (size 1 1) (layers "F.Cu") (net 2 "GND"))
  )
  (gr_rect (start 0 0) (end 20 10) (layer "Edge.Cuts") (stroke (width 0.1) (type solid)))
  (zone (net 0) (net_name "") (layers "F.Cu" "B.Cu") (name "noroute") (hatch edge 0.5) (connect_pads (clearance 0))
    (min_thickness 0.25) (keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour allowed) (footprints allowed))
    (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts (xy 9 0) (xy 11 0) (xy 11 10) (xy 9 10))))
)
"""
RULES = """(version 1)
(rule "by area" (condition "A.intersectsArea('noroute')") (constraint disallow track via))
(rule "broken" (layer inner) (condition "A.NetName != 'GND' &&& (") (constraint disallow track))
(rule "odd property" (condition "A.Frobnicate == 3") (constraint disallow track))
(rule "inner GND only" (layer inner) (condition "A.NetName != 'GND'") (constraint disallow track))
"""


def main() -> int:
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    (work / "b.kicad_pcb").write_text(BOARD)
    (work / "b.kicad_dru").write_text(RULES)
    (work / "out.kicad_dru").write_text(RULES)  # the DRC reads the rules beside the board it checks
    # SIG cannot cross (outer layers closed by the area rule, inner layers to all but GND): enough work for GND after it.
    r = subprocess.run([tm, "route", str(work / "b.kicad_pcb"), "-o", str(work / "out.kicad_pcb"), "--work", "1000000", "--variants", "1",
                        "--threads", "1", "--no-kb", "--no-gpu"], capture_output=True, text=True)
    if r.returncode not in (0, 3):
        print(r.stdout[-2000:], r.stderr[-2000:])
        return 1
    warnings = [l for l in (r.stdout + r.stderr).splitlines() if "warning:" in l]
    bad = 0
    for rule in ("broken", "odd property"):
        if not any(f"rule '{rule}'" in w for w in warnings):
            print(f"FAIL: no warning names rule '{rule}'")
            bad += 1
    for rule in ("by area", "inner GND only"):
        if any(f"rule '{rule}'" in w for w in warnings):
            print(f"FAIL: rule '{rule}', which the router applies, is warned about")
            bad += 1
    d = subprocess.run([tm, "drc", str(work / "out.kicad_pcb"), "--json", str(work / "drc.json")], capture_output=True, text=True)
    report = json.loads((work / "drc.json").read_text()) if (work / "drc.json").exists() else None
    if report is None:
        print("FAIL: no DRC report", d.stdout[-1000:], d.stderr[-1000:])
        bad += 1
    else:
        disallowed = [v for v in report.get("violations", []) if v.get("type") == "items_not_allowed"]
        if disallowed:
            print(f"FAIL: {len(disallowed)} disallowed items in the routed board")
            bad += 1
    if not re.search(r"\(via\s", (work / "out.kicad_pcb").read_text()):
        print("FAIL: GND did not cross the wall (through vias on an inner layer)")
        bad += 1
    print("\n".join(warnings))
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
