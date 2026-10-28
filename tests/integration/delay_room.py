#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Delay tuning that makes room (doc 05 §61, D152) on RoyalBlue54L-Feather-delay (bench/records/m21-b2/make_fixtures.py).

  delay_room.py <tracemaker> <work dir>

The USB pair /Debugger/D+ and D- must match within 0.5 ps. D+ leaves the connector about 8.6 ps short, and the free room beside its
tracks takes only small meanders (it reached 76.8 of 79.5 ps before D152). The router now rips the routed connections a taller meander
needs, tunes, and routes them again. The test routes the board with a work budget and judges it with TraceMaker's DRC: no skew
violation, and no connection lost to the tuning. Skips (77) without the prepared KiCad demos (bench/prepare_demos.py -> build/demos).
"""
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
DEMOS = ROOT / "build/demos"


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    if not (DEMOS / "RoyalBlue54L-Feather").is_dir():
        print(f"SKIP: {DEMOS}/RoyalBlue54L-Feather not prepared")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    subprocess.run([sys.executable, str(ROOT / "bench/records/m21-b2/make_fixtures.py"), str(DEMOS), str(work)], check=True, capture_output=True)
    src = work / "RoyalBlue54L-Feather-delay/unrouted.kicad_pcb"
    out = work / "RoyalBlue54L-Feather-delay/routed.kicad_pcb"
    for ext in (".kicad_pro", ".kicad_dru"):
        out.with_suffix(ext).write_bytes(src.with_suffix(ext).read_bytes())
    js = out.with_suffix(".json")
    p = subprocess.run([str(tm), "route", str(src), "-o", str(out), "--work", "3000000", "--time", "3600", "--soft-zones", "--no-kb",
                        "--threads", "8", "--json", str(js)], capture_output=True, text=True, env=dict(os.environ, TM_DEBUG_TUNE="1"))
    if p.returncode not in (0, 3):
        print(p.stderr[-3000:])
        print(f"FAIL: route exited {p.returncode}")
        return 1
    s = json.loads(js.read_text())
    room = [l for l in p.stderr.splitlines() if "room for /Debugger/D+" in l]
    drc = out.with_suffix(".drc.json")
    subprocess.run([str(tm), "drc", str(out), "--json", str(drc)], capture_output=True)  # exits non-zero on violations
    skew = [v["description"] for v in json.loads(drc.read_text())["violations"] if v["type"] == "skew_out_of_range"]
    print(f"routed {s['routed']}/{s['connections']}, {len(skew)} skew violation(s); " + "; ".join(l.strip() for l in room[-2:]))
    fails = []
    if skew:
        fails.append(f"skew: {skew[:2]}")
    if s["routed"] < 125:
        fails.append(f"routed {s['routed']}/{s['connections']}, fewer than the 125 routed without the room step")
    for f in fails:
        print("FAIL: " + f)
    print("PASS" if not fails else f"{len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
