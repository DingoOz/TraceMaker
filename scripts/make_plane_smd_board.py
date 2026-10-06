#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generates tests/boards/plane_smd/: a small synthetic all-SMD board for plane-aware routing (doc 05 §16).

  scripts/kicad-python scripts/make_plane_smd_board.py [out_dir]

KiCad 10's pcbnew module and its stock footprint libraries are needed (KICAD_FOOTPRINT_DIR overrides the search).
The board is unrouted. What it exercises:
  * 4 layers, every part SMD on F.Cu: In1.Cu is a filled GND plane, In2.Cu a filled +3V3 plane, and each plane layer
    has a full-board rule area "no tracks, vias allowed" (signals use F.Cu and B.Cu only). No pad touches a plane;
    every GND and +3V3 pad has to drop a via into its plane (--soft-zones).
  * U2, a 4 x 5 ball array at 0.4 mm pitch with 0.25 mm lands (0.15 mm gaps, no track passes): the inner ball B2
    carries INT to the MCU and can only leave through a via in its pad (--via-in-pad); C2, C3 and C4 are inner
    balls on the plane nets.
  * U1, a 5 x 5 mm QFN-32 MCU (0.25 mm pins at 0.5 mm pitch) with a GND exposed pad; 0402 decoupling capacitors.
    Plane vias inside those small pads are what --keep-vias-off-pads prevents; the exposed pad may take vias.
  * Y1, a 32.768 kHz crystal with its load capacitors, whose lines should stay short (--first-nets XIN,XOUT).
  * U3, an SOT-23-5 I2C sensor with pull-ups R1/R2.
Design rules (in the .kicad_pro): 0.1 mm tracks and clearance, 0.45/0.2 mm vias, minimum via 0.25/0.15 mm, 0.2 mm
hole clearance, 0.3 mm copper-to-edge. Nothing here is copied from a real product.
"""
import json
import os
import pathlib
import sys

import pcbnew

OUT = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else pathlib.Path(__file__).resolve().parents[1] / "tests/boards/plane_smd")
NAME = "plane_smd"
X0, Y0, W, H = 100.0, 100.0, 24.0, 14.0
MM = pcbnew.FromMM


def lib_dir() -> pathlib.Path:
    for d in (os.environ.get("KICAD_FOOTPRINT_DIR"), os.environ.get("KICAD10_FOOTPRINT_DIR"), "/usr/share/kicad/footprints",
              "/Applications/KiCad/KiCad.app/Contents/SharedSupport/footprints"):
        if d and pathlib.Path(d, "Capacitor_SMD.pretty").is_dir():
            return pathlib.Path(d)
    sys.exit("KiCad footprint libraries not found (set KICAD_FOOTPRINT_DIR)")


LIB = lib_dir()
IO = pcbnew.PCB_IO_KICAD_SEXPR()
b = pcbnew.BOARD()
b.SetCopperLayerCount(4)
nets = {}


def net(name):
    if name not in nets:
        n = pcbnew.NETINFO_ITEM(b, name)
        b.Add(n)
        nets[name] = n
    return nets[name]


def place(lib, fp_name, ref, x, y, pins, angle=0):
    fp = IO.FootprintLoad(str(LIB / f"{lib}.pretty"), fp_name)  # (pcbnew.FootprintLoad needs a wx app)
    fp.SetReference(ref)
    fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    fp.SetOrientationDegrees(angle)
    b.Add(fp)
    for p in fp.Pads():
        if p.GetNumber() in pins:
            p.SetNet(net(pins[p.GetNumber()]))
    return fp


def ball_array(ref, x, y, pins, rows="ABCD", cols=5, pitch=0.4, land=0.25):
    fp = pcbnew.FOOTPRINT(b)
    fp.SetReference(ref)
    fp.SetValue("WLP-20")
    fp.SetPosition(pcbnew.VECTOR2I(MM(x), MM(y)))
    b.Add(fp)
    for r, row in enumerate(rows):
        for c in range(cols):
            p = pcbnew.PAD(fp)
            p.SetNumber(f"{row}{c + 1}")
            p.SetShape(pcbnew.PAD_SHAPE_CIRCLE)
            p.SetAttribute(pcbnew.PAD_ATTRIB_SMD)
            p.SetLayerSet(pcbnew.PAD.SMDMask())
            p.SetSize(pcbnew.VECTOR2I(MM(land), MM(land)))
            p.SetFPRelativePosition(pcbnew.VECTOR2I(MM((c - (cols - 1) / 2) * pitch), MM((r - (len(rows) - 1) / 2) * pitch)))
            fp.Add(p)
            if p.GetNumber() in pins:
                p.SetNet(net(pins[p.GetNumber()]))
    return fp


def rect(layer, x0, y0, x1, y1):
    s = pcbnew.PCB_SHAPE(b)
    s.SetShape(pcbnew.SHAPE_T_RECT)
    s.SetLayer(layer)
    s.SetStart(pcbnew.VECTOR2I(MM(x0), MM(y0)))
    s.SetEnd(pcbnew.VECTOR2I(MM(x1), MM(y1)))
    s.SetWidth(MM(0.05))
    b.Add(s)


def zone(layer, name, netname=None, rule_area=False, inset=0.5):
    z = pcbnew.ZONE(b)
    z.SetLayer(layer)
    z.SetZoneName(name)
    if rule_area:
        z.SetIsRuleArea(True)
        z.SetDoNotAllowTracks(True)
        z.SetDoNotAllowVias(False)
        z.SetDoNotAllowZoneFills(False)
        z.SetDoNotAllowPads(False)
        z.SetDoNotAllowFootprints(False)
    else:
        z.SetNet(net(netname))
        z.SetLocalClearance(MM(0.2))
        z.SetMinThickness(MM(0.15))
    o = z.Outline()
    o.NewOutline()
    for px, py in ((X0 + inset, Y0 + inset), (X0 + W - inset, Y0 + inset), (X0 + W - inset, Y0 + H - inset), (X0 + inset, Y0 + H - inset)):
        o.Append(MM(px), MM(py))
    b.Add(z)


rect(pcbnew.Edge_Cuts, X0, Y0, X0 + W, Y0 + H)

# U1: QFN-32 MCU. Pins 1-8 left (top to bottom), 9-16 bottom, 17-24 right, 25-32 top; 33 exposed pad.
u1 = {"1": "XIN", "2": "XOUT", "3": "GND", "4": "+3V3", "9": "GND", "10": "+3V3", "17": "SCK", "18": "MOSI", "19": "MISO",
      "20": "CS", "21": "INT", "22": "GND", "23": "SDA", "24": "SCL", "25": "+3V3", "26": "GND", "33": "GND"}
place("Package_DFN_QFN", "QFN-32-1EP_5x5mm_P0.5mm_EP3.45x3.45mm", "U1", 109, 107, u1)
# U2: 4 x 5 ball sensor, rows A-D top to bottom, columns 1-5 left to right. B2, C2, C3, C4 are inner balls.
u2 = {"A1": "+3V3", "A2": "SCK", "A3": "MISO", "A4": "MOSI", "A5": "CS", "B1": "GND", "B2": "INT", "B5": "GND",
      "C2": "+3V3", "C3": "GND", "C4": "GND", "D2": "+3V3", "D3": "GND"}
ball_array("U2", 117.5, 107, u2)
# Y1: 32.768 kHz crystal and load capacitors, top left.
place("Crystal", "Crystal_SMD_3215-2Pin_3.2x1.5mm", "Y1", 103, 103, {"1": "XIN", "2": "XOUT"})
place("Capacitor_SMD", "C_0402_1005Metric", "C1", 101.6, 105.6, {"1": "XIN", "2": "GND"}, 90)
place("Capacitor_SMD", "C_0402_1005Metric", "C2", 104.4, 105.6, {"1": "XOUT", "2": "GND"}, 90)
# Decoupling: four around U1, two beside U2.
place("Capacitor_SMD", "C_0402_1005Metric", "C3", 105.2, 108.6, {"1": "+3V3", "2": "GND"}, 90)
place("Capacitor_SMD", "C_0402_1005Metric", "C4", 109.6, 110.9, {"1": "+3V3", "2": "GND"})
place("Capacitor_SMD", "C_0402_1005Metric", "C5", 109.6, 103.1, {"1": "+3V3", "2": "GND"})
place("Capacitor_SMD", "C_0402_1005Metric", "C6", 112.8, 109.5, {"1": "+3V3", "2": "GND"}, 90)
place("Capacitor_SMD", "C_0402_1005Metric", "C7", 117.5, 109.6, {"1": "+3V3", "2": "GND"})
place("Capacitor_SMD", "C_0402_1005Metric", "C8", 120.2, 107, {"1": "+3V3", "2": "GND"}, 90)
# U3: I2C sensor (1 SCL, 2 GND, 3 SDA, 4 alert (unused), 5 +3V3) and its pull-ups.
place("Package_TO_SOT_SMD", "SOT-23-5", "U3", 118, 102.6, {"1": "SCL", "2": "GND", "3": "SDA", "5": "+3V3"})
place("Resistor_SMD", "R_0402_1005Metric", "R1", 114.2, 102.2, {"1": "SDA", "2": "+3V3"})
place("Resistor_SMD", "R_0402_1005Metric", "R2", 114.2, 103.4, {"1": "SCL", "2": "+3V3"})
# Supply input pads.
place("TestPoint", "TestPoint_Pad_D1.0mm", "TP1", 122, 112, {"1": "+3V3"})
place("TestPoint", "TestPoint_Pad_D1.0mm", "TP2", 102, 112, {"1": "GND"})

zone(pcbnew.In1_Cu, "GND_PLANE", "GND")
zone(pcbnew.In2_Cu, "3V3_PLANE", "+3V3")
zone(pcbnew.In1_Cu, "NO_TRACKS_IN1", rule_area=True, inset=-0.5)
zone(pcbnew.In2_Cu, "NO_TRACKS_IN2", rule_area=True, inset=-0.5)
OUT.mkdir(parents=True, exist_ok=True)
pro = {
    "meta": {"filename": f"{NAME}.kicad_pro", "version": 3},
    "board": {"design_settings": {"rules": {
        "min_clearance": 0.1, "min_track_width": 0.1, "min_via_diameter": 0.25, "min_through_hole_diameter": 0.15,
        "min_via_annular_width": 0.05, "min_hole_clearance": 0.2, "min_hole_to_hole": 0.25, "min_copper_edge_clearance": 0.3,
        "allow_blind_buried_vias": False, "allow_microvias": False}}},
    "net_settings": {"meta": {"version": 4}, "classes": [{
        "name": "Default", "clearance": 0.1, "track_width": 0.1, "via_diameter": 0.45, "via_drill": 0.2, "priority": 2147483647}]},
}
(OUT / f"{NAME}.kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")
path = str(OUT / f"{NAME}.kicad_pcb")
b.Save(path)
# Fill the planes on the board as loaded from disk, with its project (filling a board built in memory crashes).
b = pcbnew.LoadBoard(path)
pcbnew.ZONE_FILLER(b).Fill(b.Zones())
b.Save(path)
(OUT / f"{NAME}.kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")  # Save() may rewrite it
(OUT / f"{NAME}.kicad_prl").unlink(missing_ok=True)  # local view settings
print(f"wrote {path} and .kicad_pro: {len(b.GetFootprints())} footprints, {len(nets)} nets")
