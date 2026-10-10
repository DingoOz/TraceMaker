#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Plane-aware routing of an all-SMD board (doc 05 §16), end to end on tests/boards/plane_smd.

  plane_smd.py <tracemaker> <work dir>

Routes the synthetic board twice at a fixed budget:
  * with the defaults, the plane fills block every via (the case the options exist for): fewer than all connections
    are routed and no via is placed;
  * with --soft-zones --via-in-pad --keep-vias-off-pads --first-nets XIN,XOUT, every connection is routed and
    - no track lies on the plane layers In1.Cu / In2.Cu (their rule areas forbid tracks, not vias);
    - GND and +3V3 reach their planes through vias: at least one via per net and at most 1.2 mm of track per pad
      on average (a short stub to a via beside each pad; the defaults wire pads to each other: ~2.7 and ~5.8 mm);
    - exactly one via lies inside an SMD pad narrower than 2 mm: the minimum via (0.25 / 0.15 mm) centred in U2.B2,
      the inner ball that has no other way out; the exposed pad U1.33 may take vias;
    - when kicad-cli is available, KiCad's DRC after refilling the zones finds no error and no unconnected item.
"""
import json
import math
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "tests/boards/plane_smd/plane_smd.kicad_pcb"
OPTS = ["--soft-zones", "--via-in-pad", "--keep-vias-off-pads", "--first-nets", "XIN,XOUT"]
NM = 1e6


def route(tm, work, name, extra):
    out = work / f"{name}.kicad_pcb"
    shutil.copy(SRC.with_suffix(".kicad_pro"), out.with_suffix(".kicad_pro"))
    r = subprocess.run([str(tm), "route", str(SRC), "-o", str(out), "--work", "3000000", "--time", "3600", "--variants", "2", "--threads", "2", "--no-kb",
                        "--no-gpu", "--json", str(work / f"{name}.route.json")] + extra, capture_output=True, text=True)
    if r.returncode not in (0, 3):
        sys.exit(f"{name}: tracemaker failed\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    res = json.loads((work / f"{name}.route.json").read_text())
    truth = work / f"{name}.truth.json"
    subprocess.run([str(tm), "inspect", str(out), "--json", str(truth)], check=True, capture_output=True)
    return out, res, json.loads(truth.read_text())


def in_pad(v, p):
    hw, hh = p["w"] / 2, p["h"] / 2
    if p["angle"] % 180:
        hw, hh = hh, hw
    return abs(v["x"] - p["x"]) <= hw and abs(v["y"] - p["y"]) <= hh


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    if not SRC.exists():
        print(f"board missing: {SRC}")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    errors = []

    _, base, base_b = route(tm, work, "default", [])
    if base["routed"] >= base["connections"] or base_b["vias"]:
        errors.append(f"defaults: expected an incomplete, via-less result, got {base['routed']}/{base['connections']} and {len(base_b['vias'])} vias")

    out, res, b = route(tm, work, "planes", OPTS)
    if res["routed"] != res["connections"]:
        errors.append(f"options: routed {res['routed']}/{res['connections']}")
    on_planes = [t for t in b["tracks"] if t["layer"] in ("In1.Cu", "In2.Cu")]
    if on_planes:
        errors.append(f"{len(on_planes)} tracks on the plane layers")
    for net in ("GND", "+3V3"):
        length = sum(math.hypot(t["ex"] - t["sx"], t["ey"] - t["sy"]) for t in b["tracks"] if t["net"] == net) / NM
        vias = sum(v["net"] == net for v in b["vias"])
        pads = sum(p["net"] == net for p in b["pads"])
        if vias == 0 or length > 1.2 * pads:
            errors.append(f"{net}: {vias} vias and {length:.1f} mm of track for {pads} pads (plane vias expected, little track)")
    small = [p for p in b["pads"] if p["attr"] == "smd" and min(p["w"], p["h"]) < 2 * NM]
    hits = [(f"{p['ref']}.{p['num']}", v) for v in b["vias"] for p in small if p["net"] == v["net"] and in_pad(v, p)]
    names = sorted({h[0] for h in hits})
    if names != ["U2.B2"]:
        errors.append(f"vias in small pads: {names} (expected only U2.B2)")
    for ref, v in hits:
        if ref == "U2.B2" and (v["size"] != 250_000 or v["drill"] != 150_000):
            errors.append(f"U2.B2 via is {v['size'] / NM}/{v['drill'] / NM} mm, expected the minimum 0.25/0.15")

    # The summary's count after the refill is the engine's own judgement of the board it wrote (doc 05 §36).
    own = work / "planes.refill.json"
    subprocess.run([str(tm), "drc", str(out), "--refill-zones", "--json", str(own)], capture_output=True)
    n_own = len(json.loads(own.read_text())["unconnected_items"])
    if res.get("unconnected_after_refill") != n_own:
        errors.append(f"summary unconnected_after_refill {res.get('unconnected_after_refill')}, drc --refill-zones on the output {n_own}")

    if shutil.which("kicad-cli"):
        drc = work / "planes.drc.json"
        subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-error", "--refill-zones", "-o", str(drc), str(out)], capture_output=True)
        d = json.loads(drc.read_text())
        if d["violations"] or d["unconnected_items"]:
            errors.append(f"KiCad DRC: {len(d['violations'])} errors, {len(d['unconnected_items'])} unconnected: "
                          + "; ".join(v["description"] for v in d["violations"][:5]))
    else:
        print("kicad-cli not found: KiCad DRC check skipped")

    print(f"defaults {base['routed']}/{base['connections']} with {len(base_b['vias'])} vias; options {res['routed']}/{res['connections']} "
          f"with {len(b['vias'])} vias")
    for e in errors:
        print("FAIL:", e)
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
