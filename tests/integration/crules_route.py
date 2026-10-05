#!/usr/bin/env python3
"""Component rules in routing (docs/15-component-rules.md §5.5, P2/P3), end to end on a PCBench board.

  crules_route.py <tracemaker> <work dir> [board]

Routes the board with --component-rules on and checks that
  * the generated keep-outs were used: no new track on a keep-out layer has a point inside a keep-out polygon
    (endpoints and points every 0.1 mm along each track, track half-width ignored: a strict subset of the exact
    check the router makes);
  * the output board does not contain the generated rule areas (they live in memory only, rule 8);
  * the sidecar <output>.tracemaker.kicad_dru was written and names the keep-out rule;
  * routing with --component-rules off writes no sidecar.
Exit 77 (skipped) when the fixture is missing.
"""
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def inside(pt, poly):
    x, y = pt
    c = False
    for i in range(len(poly)):
        x1, y1 = poly[i]
        x2, y2 = poly[(i + 1) % len(poly)]
        if (y1 > y) != (y2 > y) and x < (x2 - x1) * (y - y1) / (y2 - y1) + x1:
            c = not c
    return c


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    board = sys.argv[3] if len(sys.argv) > 3 else "CANadapter_CANadapter"
    src = FIX / board / "unrouted.kicad_pcb"
    if not src.exists():
        print(f"fixture missing: {src}")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    rules = work / "rules.json"
    subprocess.run([str(tm), "rules", str(src), "--mode", "on", "--json", str(rules)], check=True, capture_output=True)
    kos = json.loads(rules.read_text())["keepouts"]
    if not kos:
        print(f"{board}: no generated keep-out; pick a board with an SMD crystal")
        return 1
    out = work / "routed.kicad_pcb"
    side = work / "routed.tracemaker.kicad_dru"
    side.unlink(missing_ok=True)
    r = subprocess.run([str(tm), "route", str(src), "-o", str(out), "--component-rules", "on", "--threads", "2", "--variants", "2", "--work", "3000000",
                        "--no-kb", "--no-gpu"], capture_output=True, text=True)
    if r.returncode not in (0, 3):
        print(r.stdout[-2000:], r.stderr[-2000:])
        return 1
    errors = []
    if "keep-out tmk:" not in r.stdout:
        errors.append("route log does not list the keep-outs")
    if not side.exists():
        errors.append("sidecar .kicad_dru missing")
    elif "(rule \"tmk " not in side.read_text():
        errors.append("sidecar has no generated rule")
    if "tmk:" in out.read_text():
        errors.append("generated rule area written into the output board")
    truth = work / "routed.json"
    subprocess.run([str(tm), "inspect", str(out), "--json", str(truth)], check=True, capture_output=True)
    tracks = json.loads(truth.read_text())["tracks"]
    hits = 0
    for k in kos:
        poly = k["polygon_mm"]
        for t in tracks:
            if t["layer"] not in k["layers"]:
                continue
            a, b = (t["sx"] / 1e6, t["sy"] / 1e6), (t["ex"] / 1e6, t["ey"] / 1e6)
            n = max(1, int(((b[0] - a[0]) ** 2 + (b[1] - a[1]) ** 2) ** 0.5 / 0.1))
            if any(inside((a[0] + (b[0] - a[0]) * i / n, a[1] + (b[1] - a[1]) * i / n), poly) for i in range(n + 1)):
                hits += 1
    if hits:
        errors.append(f"{hits} track(s) inside a generated keep-out")
    # Off: no sidecar.
    out2 = work / "routed_off.kicad_pcb"
    side2 = work / "routed_off.tracemaker.kicad_dru"
    side2.unlink(missing_ok=True)
    subprocess.run([str(tm), "route", str(src), "-o", str(out2), "--threads", "2", "--variants", "2", "--work", "3000000", "--no-kb", "--no-gpu"],
                   capture_output=True, text=True)
    if side2.exists():
        errors.append("sidecar written with --component-rules off")
    # KiCad (when available) parses the sidecar and finds no item its generated rules disallow in the routed board.
    kicad = shutil.which("kicad-cli")
    if kicad and side.exists():
        dw = work / "kicad"
        dw.mkdir(exist_ok=True)
        shutil.copy(out, dw / "x.kicad_pcb")
        shutil.copy(side, dw / "x.kicad_dru")
        rep = dw / "drc.json"
        rep.unlink(missing_ok=True)
        subprocess.run([kicad, "pcb", "drc", "--format", "json", "-o", str(rep), str(dw / "x.kicad_pcb")], capture_output=True, text=True, timeout=600)
        if not rep.exists():
            errors.append("kicad-cli DRC with the sidecar produced no report")
        else:
            bad = [v for v in json.loads(rep.read_text()).get("violations", []) if "tmk " in v.get("description", "")]
            if bad:
                errors.append(f"KiCad DRC: {len(bad)} violation(s) of the generated rules: {bad[0]['description']}")
            else:
                print("KiCad DRC with the sidecar rules: no violation of the generated rules")
    for e in errors:
        print("FAIL:", e)
    if not errors:
        print(f"{board}: {len(kos)} keep-out(s) honoured by {len(tracks)} routed tracks; sidecar {side.name} written")
    return 1 if errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
