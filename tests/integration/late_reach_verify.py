#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Late reachability proof vs. the full A* (doc 05 §62, D154; rule 3: every accelerated path has a reference path).

  late_reach_verify.py <tracemaker> <work dir> [board]

Routes a PCBench board with the up-front pre-check off, the late proof early (20,000 expansions, no flood cap) and
--reach-verify, which runs the A* on where a proof found no path and counts any path it finds. Passes when at least
one search was proved to have no path and there were no mismatches. Exit 77 (skipped) when the fixture is missing.
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
                        "--threads", "1", "--variants", "1", "--no-kb", "--reach-check", "0", "--reach-after", "20000",
                        "--reach-after-cap", "0", "--reach-verify"], capture_output=True, text=True)
    out = p.stdout + p.stderr
    m = re.search(r"late reachability proofs: (\d+), (\d+) proved no path", out)
    if not m:
        print(out[-2000:])
        print("FAIL: no late reachability stats")
        return 1
    print(m.group(0))
    if "reachability mismatches" in out:
        print("FAIL: the A* found a path the late proof called unreachable")
        return 1
    if int(m.group(2)) == 0:
        print("FAIL: no search was proved to have no path, so nothing was verified")
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
