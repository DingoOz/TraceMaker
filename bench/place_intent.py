#!/usr/bin/env python3
"""Design-intent metrics for a placement (docs/14-placement-test-plan.md §4, "Intent").

  bench/place_intent.py board.kicad_pcb [other.kicad_pcb ...] [--json out.json]

For each board:
  decap_median_mm / decap_max_mm   for each two-pin capacitor whose two nets are a power net and a ground net that
                                   an IC (U*/IC*) also has, the distance from the capacitor's centre to the nearest
                                   such IC pad on its power net
  connectors_at_edge               connectors (reference J*/P*/CN*/USB*) with a pad within 5 mm of the board edge
                                   (bounding box of Edge.Cuts; bodies overhang pads), and the total
  crystal_median_mm / crystal_max  crystals/oscillators (Y*/X*/XTAL*/OSC*) to the nearest IC pad they connect to
  passive_rotation_ok              share of two-pin passives (R/C/L/D/F/FB) at a multiple of 90 degrees
"""
import argparse
import json
import math
import pathlib
import re
import statistics
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = ROOT / "build/release/src/app/tracemaker"
GND = re.compile(r"(^|[/_-])(A|D|P)?GND|VSS|^0V$", re.I)
PWR = re.compile(r"VCC|VDD|VBAT|VIN|VBUS|\+\d|^\d+V\d*$|3V3|5V|1V8|12V|VSYS|PWR", re.I)


def board_json(path: pathlib.Path) -> dict:
    out = ROOT / "build/quality" / (path.resolve().as_posix().replace("/", "_")[-150:] + ".intent.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(TM), "inspect", str(path), "--json", str(out)], check=True, capture_output=True)
    return json.loads(out.read_text())


def edge_box(path: pathlib.Path):
    """Bounding box of the board-level Edge.Cuts graphics, in nm (balanced-paren scan; blocks nest two deep)."""
    txt = path.read_text(errors="replace")
    xs, ys = [], []
    for m in re.finditer(r"\((gr_line|gr_arc|gr_rect|gr_circle|gr_poly)\b", txt):
        depth, i = 0, m.start()
        while True:
            c = txt[i]
            depth += (c == "(") - (c == ")")
            i += 1
            if depth == 0:
                break
        blk = txt[m.start():i]
        if '"Edge.Cuts"' not in blk and " Edge.Cuts)" not in blk:
            continue
        for x, y in re.findall(r"\((?:start|end|mid|center|xy)\s+(-?[\d.]+)\s+(-?[\d.]+)\)", blk):
            xs.append(float(x) * 1e6)
            ys.append(float(y) * 1e6)
    return (min(xs), min(ys), max(xs), max(ys)) if xs else None


def metrics(path: pathlib.Path) -> dict:
    d = board_json(path)
    pads_by_ref = {}
    for p in d["pads"]:
        pads_by_ref.setdefault(p["ref"], []).append(p)
    fps = {f["ref"]: f for f in d["footprints"]}
    ic = [r for r in fps if re.match(r"^(U|IC)\d", r)]
    dist = lambda a, b: math.hypot(a[0] - b[0], a[1] - b[1]) / 1e6  # noqa: E731
    # Decoupling capacitors.
    decap = []
    for r, f in fps.items():
        if not re.match(r"^C\d", r) or len(pads_by_ref.get(r, [])) != 2:
            continue
        nets = {p["net"] for p in pads_by_ref[r]}
        pwr = [n for n in nets if n and PWR.search(n) and not GND.search(n)]
        gnd = [n for n in nets if n and GND.search(n)]
        if len(pwr) != 1 or len(gnd) != 1:
            continue
        cands = [(p["x"], p["y"]) for u in ic for p in pads_by_ref.get(u, []) if p["net"] == pwr[0]]
        if not cands or not any(p["net"] == gnd[0] for u in ic for p in pads_by_ref.get(u, [])):
            continue
        decap.append(min(dist((f["x"], f["y"]), c) for c in cands))
    # Connectors at the edge.
    box = edge_box(path)
    conn = [r for r, f in fps.items() if re.match(r"^(J|P|CN|USB)\d", r)]
    at_edge = 0
    if box:
        for r in conn:
            ps = pads_by_ref.get(r, [])
            if not ps:
                continue
            m = min(min(p["x"] - box[0], box[2] - p["x"], p["y"] - box[1], box[3] - p["y"]) for p in ps) / 1e6
            at_edge += m <= 5.0
    # Crystals.
    xtal = []
    for r, f in fps.items():
        if not re.match(r"^(Y|X|XTAL|OSC)\d", r):
            continue
        nets = {p["net"] for p in pads_by_ref.get(r, []) if p["net"] and not GND.search(p["net"])}
        cands = [(p["x"], p["y"]) for u in ic for p in pads_by_ref.get(u, []) if p["net"] in nets]
        if cands:
            xtal.append(min(dist((f["x"], f["y"]), c) for c in cands))
    # Passive rotations.
    pas = [f for r, f in fps.items() if re.match(r"^(R|C|L|D|F|FB)\d", r) and len(pads_by_ref.get(r, [])) == 2]
    rot_ok = sum(1 for f in pas if abs(((f["angle"] % 90) + 90) % 90) < 0.01 or abs(((f["angle"] % 90) + 90) % 90 - 90) < 0.01)
    med = lambda v: round(statistics.median(v), 2) if v else None  # noqa: E731
    return {"board": str(path), "decaps": len(decap), "decap_median_mm": med(decap), "decap_max_mm": round(max(decap), 2) if decap else None,
            "connectors": len(conn), "connectors_at_edge": at_edge, "crystals": len(xtal), "crystal_median_mm": med(xtal),
            "crystal_max_mm": round(max(xtal), 2) if xtal else None,
            "passive_rotation_ok": round(rot_ok / len(pas), 3) if pas else None}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--json")
    a = ap.parse_args()
    rows = [metrics(pathlib.Path(b)) for b in a.boards]
    for r in rows:
        print(json.dumps(r))
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
