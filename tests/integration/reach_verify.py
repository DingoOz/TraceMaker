#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Reachability pre-check vs. the full A* (doc 05 §13; rule 3: every accelerated path has a reference path).

  reach_verify.py <tracemaker> <work dir> [board]

Routes a PCBench board with the pre-check on every strict search and --reach-verify, which runs the A* even
where the flood fill proved no path and counts any path it finds. Passes when at least one search was proved
unreachable and there were no mismatches. Exit 77 (skipped) when the fixture is missing.
"""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    board = sys.argv[3] if len(sys.argv) > 3 else "sbc_sbc"
    src = FIX / board / "unrouted.kicad_pcb"
    if not src.exists():
        print(f"skip: {src} missing (scripts/fetch_fixtures.sh)")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([str(tm), "route", str(src), "-o", str(work / "out.kicad_pcb"), "--work", "15000000", "--time", "3600",
                        "--threads", "1", "--variants", "1", "--no-kb", "--reach-check", "2", "--reach-verify"], capture_output=True, text=True)
    out = p.stdout + p.stderr
    m = re.search(r"reachability checks: (\d+), (\d+) proved unreachable", out)
    if not m:
        print(out[-2000:])
        print("FAIL: no reachability stats")
        return 1
    print(m.group(0))
    if "reachability mismatches" in out or "verified (0 mismatches)" not in out:
        print("FAIL: the A* found a path the pre-check called unreachable")
        return 1
    if int(m.group(2)) == 0:
        print("FAIL: no search was proved unreachable, so nothing was verified")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
