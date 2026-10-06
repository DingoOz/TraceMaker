#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Roadmap M12 gate for coupled differential pairs (doc 05 §15, D50).

  diff_pair.py <tracemaker> <workdir>

Routes PCBench 4-port-usb-hub (five USB 2.0 pairs D0..D4 +/-) with --diff-pairs at a fixed work budget, twice:
the outputs must be identical (determinism), every connection routed, every pair coupled over at least 80 % of its
length at no less than its gap, and, when kicad-cli is available, KiCad's DRC must find no unconnected items and no
errors on routed copper. Exit 77 when the fixture is missing (TM_FIXTURES overrides the PCBench directory).
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIX = pathlib.Path(os.environ.get("TM_FIXTURES", ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"))
SRC = FIX / "4-port-usb-hub_4port-usb-hub/unrouted.kicad_pcb"


def main() -> int:
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    if not SRC.exists():
        print("fixture missing: skipped")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    outs = []
    for k, threads in ((1, "1"), (2, "2")):
        out = work / f"hub_pairs_{k}.kicad_pcb"
        js = work / f"hub_pairs_{k}.json"
        subprocess.run([tm, "route", str(SRC), "-o", str(out), "--diff-pairs", "--work", "15000000", "--time", "3600", "--variants", "1", "--threads", threads,
                        "--no-kb", "--no-gpu", "--json", str(js)], capture_output=True)
        outs.append((out, json.loads(js.read_text())))
    ok = True
    if outs[0][0].read_bytes() != outs[1][0].read_bytes():
        print("FAIL: two runs differ")
        ok = False
    res = outs[0][1]
    print(f"routed {res['routed']}/{res['connections']}")
    ok &= res["routed"] == res["connections"]
    pj = work / "pairs.json"
    subprocess.run([tm, "pairs", str(outs[0][0]), "--json", str(pj)], check=True, capture_output=True)
    pairs = json.loads(pj.read_text())["pairs"]
    ok &= len(pairs) == 5
    for p in pairs:
        good = p["coupled_share"] >= 0.8 and p["gap_min_mm"] >= p["target_gap_mm"] - 1e-3
        print(f"{p['net_a']}/{p['net_b']}: coupled {100 * p['coupled_share']:.1f} %, gap {p['gap_min_mm']:.3f} mm (target {p['target_gap_mm']:.3f}), "
              f"skew {p['skew_mm']:.2f} mm {'' if good else ' FAIL'}")
        ok &= good
    if shutil.which("kicad-cli"):
        drc = work / "drc.json"
        subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", str(drc), str(outs[0][0])], capture_output=True)
        v = json.loads(drc.read_text())
        bad = [x for x in v["violations"] if x["severity"] == "error" and any(i["description"].startswith(("Track", "Via")) for i in x["items"])]
        print(f"KiCad DRC: unconnected {len(v['unconnected_items'])}, errors on routed copper {len(bad)}")
        ok &= not bad and not v["unconnected_items"]
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
