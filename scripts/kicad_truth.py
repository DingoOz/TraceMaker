# SPDX-License-Identifier: GPL-3.0-or-later
"""Dumps KiCad's own view of a board as JSON, for parser and geometry tests.

Runs inside the KiCad image: scripts/kicad-python scripts/kicad_truth.py board.kicad_pcb out.json
All coordinates are integer nanometres (KiCad internal units); angles are degrees.
"""
import json
import sys

import pcbnew


def layer_names(board, lset):
    enabled = board.GetEnabledLayers()
    return [board.GetStandardLayerName(l) for l in lset.CuStack() if enabled.Contains(l)]


def main(path, out):
    b = pcbnew.LoadBoard(path)
    cu = [b.GetStandardLayerName(l) for l in b.GetEnabledLayers().CuStack()]
    pad_attr = {pcbnew.PAD_ATTRIB_PTH: "thru_hole", pcbnew.PAD_ATTRIB_SMD: "smd",
                pcbnew.PAD_ATTRIB_CONN: "connect", pcbnew.PAD_ATTRIB_NPTH: "np_thru_hole"}
    data = {"copper_layers": cu, "nets": sorted(str(n) for n in b.GetNetsByName().keys() if str(n)), "footprints": [],
            "pads": [], "tracks": [], "arcs": [], "vias": [], "zones": []}
    for fp in b.GetFootprints():
        pos = fp.GetPosition()
        data["footprints"].append({
            "ref": fp.GetReference(), "x": pos.x, "y": pos.y, "angle": fp.GetOrientationDegrees(),
            "back": fp.IsFlipped(), "locked": fp.IsLocked(), "pads": len(fp.Pads())})
        for p in fp.Pads():
            pp = p.GetPosition()
            sz = p.GetSize(pcbnew.F_Cu) if hasattr(p, "GetSize") else p.GetSize()
            dr = p.GetDrillSize()
            data["pads"].append({
                "ref": fp.GetReference(), "num": p.GetNumber(), "x": pp.x, "y": pp.y,
                "angle": p.GetOrientationDegrees(), "w": sz.x, "h": sz.y, "drill_w": dr.x, "drill_h": dr.y,
                "attr": pad_attr.get(p.GetAttribute(), "?"), "net": p.GetNetname(),
                "layers": layer_names(b, p.GetLayerSet())})
    for t in b.GetTracks():
        cls = t.GetClass()
        if cls == "PCB_VIA":
            vt = t.GetViaType()
            kind = {pcbnew.VIATYPE_THROUGH: "through", pcbnew.VIATYPE_MICROVIA: "micro"}.get(vt, "blind")
            data["vias"].append({"x": t.GetPosition().x, "y": t.GetPosition().y, "size": t.GetWidth(pcbnew.F_Cu),
                                 "drill": t.GetDrillValue(), "top": b.GetStandardLayerName(t.TopLayer()),
                                 "bottom": b.GetStandardLayerName(t.BottomLayer()), "type": kind, "net": t.GetNetname()})
        elif cls == "PCB_ARC":
            data["arcs"].append({"sx": t.GetStart().x, "sy": t.GetStart().y, "mx": t.GetMid().x, "my": t.GetMid().y,
                                 "ex": t.GetEnd().x, "ey": t.GetEnd().y, "width": t.GetWidth(),
                                 "layer": b.GetStandardLayerName(t.GetLayer()), "net": t.GetNetname()})
        else:
            data["tracks"].append({"sx": t.GetStart().x, "sy": t.GetStart().y, "ex": t.GetEnd().x, "ey": t.GetEnd().y,
                                   "width": t.GetWidth(), "layer": b.GetStandardLayerName(t.GetLayer()), "net": t.GetNetname()})
    for z in b.Zones():
        data["zones"].append({"net": z.GetNetname(), "rule_area": z.GetIsRuleArea(),
                              "layers": [b.GetStandardLayerName(l) for l in z.GetLayerSet().Seq()],
                              "outline_points": z.Outline().TotalVertices()})
    bb = b.GetBoardEdgesBoundingBox()
    data["edges_bbox"] = [bb.GetX(), bb.GetY(), bb.GetRight(), bb.GetBottom()]
    with open(out, "w") as f:
        json.dump(data, f)
    print(f"{path}: {len(data['footprints'])} footprints, {len(data['pads'])} pads, {len(data['tracks'])} tracks, "
          f"{len(data['arcs'])} arcs, {len(data['vias'])} vias, {len(data['zones'])} zones")


if __name__ == "__main__":
    main(sys.argv[1], sys.argv[2])
