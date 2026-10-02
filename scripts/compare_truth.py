#!/usr/bin/env python3
"""Compares two board JSON dumps in the kicad_truth schema (KiCad's view vs TraceMaker's view).

Usage: compare_truth.py kicad.json ours.json   — exits 1 and lists differences if they disagree.
"""
import json
import sys
from collections import Counter


def angle(a: float) -> float:
    return round(a % 360.0, 4) % 360.0


def main(a_path: str, b_path: str) -> int:
    a, b = json.load(open(a_path)), json.load(open(b_path))
    problems: list[str] = []
    if a["copper_layers"] != b["copper_layers"]:
        problems.append(f"copper layers {a['copper_layers']} vs {b['copper_layers']}")
    if set(a["nets"]) != set(b["nets"]):
        problems.append(f"nets differ: only kicad {sorted(set(a['nets']) - set(b['nets']))[:5]}, only ours {sorted(set(b['nets']) - set(a['nets']))[:5]}")

    def fp_key(f): return (f["ref"], f["x"], f["y"], angle(f["angle"]), f["back"], f["pads"])
    def pad_key(p): return (p["ref"], p["num"], p["x"], p["y"], angle(p["angle"]), p["w"], p["h"], p["drill_w"], p["net"], tuple(p["layers"]))
    def trk_key(t): return (t["sx"], t["sy"], t["ex"], t["ey"], t["width"], t["layer"], t["net"])
    def via_key(v): return (v["x"], v["y"], v["size"], v["drill"], v["top"], v["bottom"], v["net"])
    for name, key in (("footprints", fp_key), ("pads", pad_key), ("tracks", trk_key), ("vias", via_key)):
        ca, cb = Counter(map(key, a[name])), Counter(map(key, b[name]))
        if ca != cb:
            only_a, only_b = list((ca - cb).elements()), list((cb - ca).elements())
            problems.append(f"{name}: {len(only_a)} only in kicad, {len(only_b)} only in ours; e.g. kicad {only_a[:2]} ours {only_b[:2]}")
    if len(a["zones"]) != len(b["zones"]):
        problems.append(f"zones {len(a['zones'])} vs {len(b['zones'])}")
    for p in problems:
        print("DIFF", p)
    print("MATCH" if not problems else f"{len(problems)} difference(s)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1], sys.argv[2]))
