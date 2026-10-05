#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks TraceMaker's footprint flip (io::BoardEditor::flip_footprint) against KiCad's own FOOTPRINT::Flip.

  build/release/src/place/tracemaker-place in.kicad_pcb -o ours.kicad_pcb --debug-flip all
  scripts/kicad-python scripts/flip_check.py in.kicad_pcb ours.kicad_pcb [--json out.json]

For every footprint that is on the other side in `ours`, the original is flipped in memory by pcbnew (top/bottom,
about its own position) and compared item by item with ours, as KiCad reads both: footprint side, position and
orientation; per pad position, orientation, size, shape, layers, drill and the exact copper polygon (XOR area);
per graphic and text layer, bounding box, angle, mirroring and justification; the courtyards. Runs inside the
KiCad 10 Docker image (pcbnew module). Exit 0 if everything matches.
"""
import argparse
import json
import sys

import pcbnew

TOL = 2  # nm: both sides compute rotated geometry with their own rounding


def norm(a):
    a = a % 360.0
    return 0.0 if abs(a - 360.0) < 1e-6 else a


def bbox(b):
    return (b.GetX(), b.GetY(), b.GetRight(), b.GetBottom())


def close(a, b, tol=TOL):
    return all(abs(x - y) <= tol for x, y in zip(a, b))


def xor_area(a, b):
    x = pcbnew.SHAPE_POLY_SET(a)
    x.BooleanXor(b)
    return x.Area()


def pad_poly(pad, layer):
    ps = pcbnew.SHAPE_POLY_SET()
    pad.TransformShapeToPolygon(ps, layer, 0, 1000, pcbnew.ERROR_INSIDE)
    return ps


def compare(fk, fo, out):
    errs = []
    if fk.GetLayer() != fo.GetLayer():
        errs.append(f"side {fk.GetLayerName()} vs {fo.GetLayerName()}")
    if not close((fk.GetPosition().x, fk.GetPosition().y), (fo.GetPosition().x, fo.GetPosition().y), 0):
        errs.append("position")
    if abs(norm(fk.GetOrientationDegrees()) - norm(fo.GetOrientationDegrees())) > 1e-6:
        errs.append(f"orientation {fk.GetOrientationDegrees()} vs {fo.GetOrientationDegrees()}")
    pk, po = list(fk.Pads()), list(fo.Pads())
    if len(pk) != len(po):
        errs.append("pad count")
    for a, b in zip(pk, po):
        tag = f"pad {a.GetNumber()}"
        if not close((a.GetPosition().x, a.GetPosition().y), (b.GetPosition().x, b.GetPosition().y)):
            errs.append(f"{tag} position {a.GetPosition()} vs {b.GetPosition()}")
        if abs(norm(a.GetOrientationDegrees()) - norm(b.GetOrientationDegrees())) > 1e-6:
            errs.append(f"{tag} orientation {a.GetOrientationDegrees()} vs {b.GetOrientationDegrees()}")
        # Layers the board has: KiCad's in-memory flip of "*.Cu" keeps only the board's copper count, a load keeps all.
        brd = fo.GetBoard()
        if [x for x in a.GetLayerSet().Seq() if brd.IsLayerEnabled(x)] != [x for x in b.GetLayerSet().Seq() if brd.IsLayerEnabled(x)]:
            errs.append(f"{tag} layers")
        if a.GetDrillSize() != b.GetDrillSize():
            errs.append(f"{tag} drill")
        for layer in a.GetLayerSet().CuStack():
            d = xor_area(pad_poly(a, layer), pad_poly(b, layer))
            if d > 1e6:  # 1 µm² (rounding along the outline only)
                errs.append(f"{tag} copper on {pcbnew.LayerName(layer)} differs by {d / 1e6:.1f} um^2")
        if not close(bbox(a.GetBoundingBox()), bbox(b.GetBoundingBox())):
            errs.append(f"{tag} bbox {bbox(a.GetBoundingBox())} vs {bbox(b.GetBoundingBox())}")
    gk = list(fk.GraphicalItems()) + list(fk.GetFields())
    go = list(fo.GraphicalItems()) + list(fo.GetFields())
    if len(gk) != len(go):
        errs.append("item count")
    for a, b in zip(gk, go):
        tag = f"{a.GetClass()} on {a.GetLayerName()}"
        if a.GetClass() == "PCB_FIELD" and not a.GetText() and not a.IsVisible():
            continue  # empty hidden field KiCad adds when loading a KiCad 5 file (not in the file, not ours)
        if a.GetLayer() != b.GetLayer():
            errs.append(f"{tag} layer {b.GetLayerName()}")
        if hasattr(a, "GetTextAngleDegrees"):
            if a.IsVisible() or b.IsVisible():
                if abs(norm(a.GetTextAngleDegrees()) - norm(b.GetTextAngleDegrees())) > 1e-6:
                    errs.append(f"{tag} '{a.GetText()}' angle {a.GetTextAngleDegrees()} vs {b.GetTextAngleDegrees()}")
                if a.IsMirrored() != b.IsMirrored():
                    errs.append(f"{tag} '{a.GetText()}' mirrored {a.IsMirrored()} vs {b.IsMirrored()}")
                if a.GetHorizJustify() != b.GetHorizJustify() or a.GetVertJustify() != b.GetVertJustify():
                    errs.append(f"{tag} '{a.GetText()}' justification")
                if not close((a.GetTextPos().x, a.GetTextPos().y), (b.GetTextPos().x, b.GetTextPos().y)):
                    errs.append(f"{tag} '{a.GetText()}' position {a.GetTextPos()} vs {b.GetTextPos()}")
        elif not close(bbox(a.GetBoundingBox()), bbox(b.GetBoundingBox())):
            errs.append(f"{tag} bbox {bbox(a.GetBoundingBox())} vs {bbox(b.GetBoundingBox())}")
    for layer in (pcbnew.F_CrtYd, pcbnew.B_CrtYd):
        ck, co = fk.GetCourtyard(layer), fo.GetCourtyard(layer)
        if ck.OutlineCount() != co.OutlineCount():
            errs.append(f"courtyard {pcbnew.LayerName(layer)} outlines {ck.OutlineCount()} vs {co.OutlineCount()}")
        elif ck.OutlineCount() and xor_area(ck, co) > 1e6:
            errs.append(f"courtyard {pcbnew.LayerName(layer)} differs")
    out.append({"ref": fk.GetReference(), "lib": str(fk.GetFPID().GetLibItemName()), "pads": len(pk), "errors": errs})
    return not errs


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("original")
    ap.add_argument("ours")
    ap.add_argument("--json")
    a = ap.parse_args()
    bk = pcbnew.LoadBoard(a.original)
    bo = pcbnew.LoadBoard(a.ours)
    fks, fos = list(bk.GetFootprints()), list(bo.GetFootprints())
    if len(fks) != len(fos):
        print("footprint count differs")
        return 1
    for fk in fks:
        fk.BuildCourtyardCaches()
    out, bad = [], 0
    for fk, fo in zip(fks, fos):
        if fk.GetLayer() == fo.GetLayer():
            continue  # not flipped by us
        fk.Flip(fk.GetPosition(), pcbnew.FLIP_DIRECTION_TOP_BOTTOM)
        fk.BuildCourtyardCaches()
        fo.BuildCourtyardCaches()
        if not compare(fk, fo, out):
            bad += 1
    for r in out:
        if r["errors"]:
            print(f"{r['ref']} ({r['lib']}): " + "; ".join(r["errors"][:6]))
    print(f"{len(out)} flipped footprint(s) compared, {bad} differ from KiCad's flip")
    if a.json:
        with open(a.json, "w") as f:
            json.dump({"compared": len(out), "differ": bad, "footprints": out}, f, indent=1)
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
