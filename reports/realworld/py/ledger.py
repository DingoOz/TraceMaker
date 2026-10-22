#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compromise ledger of a routed board.

  ledger.py ROUTED.kicad_pcb BASELINE.kicad_pcb OUT.json [--layers N]

ROUTED carries the board's OWN rules (sweep.py puts them back), BASELINE is the board it was routed from (fixture or fixture with the designer's
planes). KiCad 10.0.6 judges both (zones refilled). What is in ROUTED's DRC and not in BASELINE's is what the routing (or the zone refill it caused)
added: each such violation is one compromise, recorded with the rule, the required and the actual value, the nets, the layer and the place, and
classified against the manufacturer's stated capability (JLCPCB standard service, 1 oz outer copper; see the report's chapter on limits).
A second, geometric survey lists every pair of tracks and every track-pad pair whose gap is under the board's clearance, with the length they run
side by side: that is what the parasitic extraction needs.
"""
from __future__ import annotations

import json
import math
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import geomx
import kgeom

ROOT = pathlib.Path(__file__).resolve().parents[3]
KICAD = ROOT / "scripts" / "kicad-cli"

# JLCPCB standard capability (https://jlcpcb.com/capabilities/pcb-capabilities, read 2026-10-17), 1 oz outer copper, green mask, mm.
FAB = {
    "track_space_2L": 0.10, "track_space_4L": 0.09, "track_width_2L": 0.10, "track_width_4L": 0.09,
    "pad_space": 0.15, "edge": 0.20, "annular_pth": 0.15, "mask_web": 0.10, "via_hole": 0.15, "via_pad": 0.25,
}
COMFORT = 0.15  # at or above this a gap is conventional (6 mil) whatever the fab limit; below it the margin to the limit is what counts


def kicad_drc(board: pathlib.Path, out: pathlib.Path) -> dict:
    if not out.exists() or out.stat().st_mtime < board.stat().st_mtime:
        subprocess.run([str(KICAD), "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "--refill-zones", "--units", "mm", "-o", str(out), str(board)],
                       check=False, capture_output=True)
    return json.loads(out.read_text())


IGNORE = {"lib_footprint_issues", "lib_footprint_mismatch", "text_height", "text_thickness", "silk_over_copper", "silk_overlap", "silk_edge_clearance",
          "mirrored_text_on_front_layer", "nonmirrored_text_on_back_layer", "missing_footprint", "extra_footprint", "duplicate_footprints", "footprint_type_mismatch",
          "footprint_filters", "unresolved_variable", "item_on_disabled_layer", "invalid_outline"}  # library, silkscreen and text checks: not routing


def key(v: dict) -> tuple:
    items = sorted((re.sub(r", length [0-9.]+ mm", "", i["description"]), round(i["pos"]["x"], 1), round(i["pos"]["y"], 1)) for i in v["items"])
    return (v["type"], tuple(items))


def nets_of(v: dict) -> list[str]:
    s = set()
    for i in v["items"]:
        for m in re.finditer(r"\[([^\]]*)\]", i["description"]):
            s.add(m.group(1))
    return sorted(s)


def classify(v: dict, layers: int) -> dict:
    """Required and actual value of a violation, and where the actual sits against the fab's stated capability."""
    m = re.search(r"([0-9.]+) mm; actual ([0-9.]+) mm", v["description"])
    req = float(m.group(1)) if m else None
    act = float(m.group(2)) if m else None
    t = v["type"]
    kinds = " ".join(i["description"] for i in v["items"])
    out = {"required": req, "actual": act, "fab_min": None, "class": "judgement", "why": ""}
    if t == "clearance" and act is not None:
        pad_pad = len(re.findall(r"\bPad\b|\bPTH pad\b|\bNPTH pad\b", kinds)) >= 2 and "Track" not in kinds and "Via" not in kinds and "Zone" not in kinds
        fmin = FAB["pad_space"] if pad_pad else FAB["track_space_4L" if layers >= 4 else "track_space_2L"]
        out["fab_min"] = fmin
        if "Zone" in kinds:
            out["why"] = "copper pour to copper"
        if act < fmin:
            out["class"] = "D below fab capability"
        elif act < 0.127:
            out["class"] = "C at the fab limit"
        elif act < COMFORT:
            out["class"] = "B reduced margin"
        else:
            out["class"] = "A conventional (>= 0.15 mm)"
    elif t == "copper_edge_clearance" and act is not None:
        out["fab_min"] = FAB["edge"]
        out["class"] = "D below fab capability" if act < FAB["edge"] else ("B reduced margin" if act < 0.3 else "A conventional (>= 0.3 mm)")
    elif t == "annular_width" and act is not None:
        out["fab_min"] = FAB["annular_pth"]
        out["class"] = "D below fab capability" if act < 0.10 else ("C at the fab limit" if act < FAB["annular_pth"] else "A conventional")
    elif t == "track_width" and act is not None:
        fmin = FAB["track_width_4L" if layers >= 4 else "track_width_2L"]
        out["fab_min"] = fmin
        out["class"] = "D below fab capability" if act < fmin else ("C at the fab limit" if act < 0.127 else ("B reduced margin" if act < COMFORT else "A conventional"))
    elif t in ("hole_clearance", "hole_to_hole"):
        out["class"] = "judgement"
        out["why"] = "the fab data read give no copper-to-hole figure; drill position tolerance is about +-0.1 mm"
    elif t == "solder_mask_bridge":
        out["fab_min"] = FAB["mask_web"]
        out["class"] = "mask web removed"
        out["why"] = "the mask web between the openings is below the fab's 0.10 mm: the fab merges the openings, so copper between the pads is exposed to solder"
    elif t == "courtyards_overlap":
        out["class"] = "assembly"
        out["why"] = "part courtyards overlap: pick-and-place and rework spacing, not electrical"
    elif t == "unconnected_items":
        out["class"] = "open"
    return out


def survey(board: kgeom.Board, clearance: float, layers: int):
    """Track-track and track-pad gaps under `clearance`, with the length run side by side."""
    pairs = []
    tr = board.tracks
    pads = [p for part in board.parts for p in part.pads if p.kind != "np_thru_hole"]
    # grid of tracks
    cell = 3.0
    grid: dict = {}
    for i, t in enumerate(tr):
        for gx in range(int(min(t.x1, t.x2) // cell) - 1, int(max(t.x1, t.x2) // cell) + 2):
            for gy in range(int(min(t.y1, t.y2) // cell) - 1, int(max(t.y1, t.y2) // cell) + 2):
                grid.setdefault((gx, gy, t.layer), []).append(i)
    seen = set()
    for i, a in enumerate(tr):
        cand = set()
        for gx in range(int(min(a.x1, a.x2) // cell) - 1, int(max(a.x1, a.x2) // cell) + 2):
            for gy in range(int(min(a.y1, a.y2) // cell) - 1, int(max(a.y1, a.y2) // cell) + 2):
                cand.update(grid.get((gx, gy, a.layer), []))
        for j in cand:
            if j <= i or (i, j) in seen:
                continue
            seen.add((i, j))
            b = tr[j]
            if a.net == b.net:
                continue
            d = geomx.seg_seg((a.x1, a.y1, a.x2, a.y2), (b.x1, b.y1, b.x2, b.y2))
            gap = d - a.w / 2 - b.w / 2
            if gap < clearance - 1e-3:
                run, cd = geomx.parallel_run((a.x1, a.y1, a.x2, a.y2), (b.x1, b.y1, b.x2, b.y2), clearance + (a.w + b.w) / 2 + 0.05)
                pairs.append({"kind": "track-track", "layer": a.layer, "gap": round(max(gap, 0.0), 4), "run": round(run, 3), "nets": [board.nets.get(a.net, ""), board.nets.get(b.net, "")],
                              "w": [a.w, b.w], "at": [round((a.x1 + a.x2) / 2, 2), round((a.y1 + a.y2) / 2, 2)]})
    pad_by_layer: dict = {}
    for p in pads:
        for ly in p.layers:
            if ly.endswith(".Cu") or ly == "*.Cu":
                pad_by_layer.setdefault(ly, []).append(p)
    for t in tr:
        for p in pad_by_layer.get(t.layer, []) + pad_by_layer.get("*.Cu", []):
            if p.net == t.net:
                continue
            if abs(p.x - (t.x1 + t.x2) / 2) > 6 and abs(p.y - (t.y1 + t.y2) / 2) > 6:
                continue
            g = geomx.seg_pad_gap((t.x1, t.y1, t.x2, t.y2), p) - t.w / 2
            if g < clearance - 1e-3:
                pairs.append({"kind": "track-pad", "layer": t.layer, "gap": round(max(g, 0.0), 4), "run": 0.0, "nets": [board.nets.get(t.net, ""), p.netname],
                              "w": [t.w, 0], "at": [round(p.x, 2), round(p.y, 2)], "pad": f"{p.ref}.{p.num}"})
    return pairs


def main() -> int:
    routed, baseline, out = map(pathlib.Path, sys.argv[1:4])
    layers = int(sys.argv[sys.argv.index("--layers") + 1]) if "--layers" in sys.argv else 2
    d_r = kicad_drc(routed, routed.with_suffix(".drc.json"))
    d_b = kicad_drc(baseline, baseline.with_suffix(".drc.json"))
    base_keys = {key(v) for v in d_b["violations"]}
    added, inherited = [], {}
    for v in d_r["violations"]:
        if v["type"] in IGNORE:
            continue
        if key(v) in base_keys:
            inherited[v["type"]] = inherited.get(v["type"], 0) + 1
            continue
        c = classify(v, layers)
        added.append({"type": v["type"], "severity": v["severity"], "nets": nets_of(v), "layer": next((m.group(1) for i in v["items"] for m in [re.search(r" on ([A-Za-z0-9.]+Cu)", i["description"])] if m), ""),
                      "pos": [round(v["items"][0]["pos"]["x"], 2), round(v["items"][0]["pos"]["y"], 2)] if v["items"] else None,
                      "items": [i["description"] for i in v["items"]], **c})
    brd = kgeom.load(str(routed))
    rules = brd.rules
    near = survey(brd, rules.get("trace_clearance", 0.2), layers)
    res = {"routed": str(routed), "baseline": str(baseline), "unconnected": len(d_r["unconnected_items"]), "unconnected_baseline": len(d_b["unconnected_items"]),
           "added": added, "inherited_counts": inherited, "tracks": len(brd.tracks), "vias": len(brd.vias), "near": near,
           "kicad": d_r.get("kicad_version", "")}
    out.write_text(json.dumps(res, indent=1))
    by = {}
    for a in added:
        by.setdefault((a["type"], a["class"]), 0)
        by[(a["type"], a["class"])] += 1
    print(f"unconnected {res['unconnected']} (baseline {res['unconnected_baseline']}); added violations {len(added)}; inherited {sum(inherited.values())}; near-misses {len(near)}")
    for k, n in sorted(by.items()):
        print(f"  {n:4d}  {k[0]:24s} {k[1]}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
