#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bit-identical routing at any thread count (requirement N3, doc 02 §6.2, decision D47).

  thread_determinism.py <tracemaker> <work dir> [board] [work]

Routes a PCBench board with a work budget and the default portfolio (all variants) at --threads 1, 2 and 4 and
compares the output files byte for byte, and the winning variant. Exit 77 (skipped) when the fixture is missing.
"""
import json
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    board = sys.argv[3] if len(sys.argv) > 3 else "CANadapter_CANadapter"
    budget = sys.argv[4] if len(sys.argv) > 4 else "1000000"
    src = FIX / board / "unrouted.kicad_pcb"
    if not src.exists():
        print(f"skip: {src} missing (scripts/fetch_fixtures.sh)")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    outs = {}
    for threads in (1, 2, 4):
        out = work / f"t{threads}.kicad_pcb"
        js = work / f"t{threads}.json"
        out.unlink(missing_ok=True)
        # --no-gpu: the GPUs are shared and may be full; GPU/CPU field equality is tested separately.
        p = subprocess.run([str(tm), "route", str(src), "-o", str(out), "--work", budget, "--time", "3600", "--seed", "7",
                            "--threads", str(threads), "--no-kb", "--no-gpu", "--json", str(js)], capture_output=True, text=True)
        if p.returncode not in (0, 3) or not out.exists():
            print(p.stdout[-2000:], p.stderr[-2000:])
            print(f"FAIL: route at --threads {threads} exited {p.returncode}")
            return 1
        m = re.search(r"portfolio: (\d+) variants on (\d+) thread", p.stdout)
        if not m or int(m.group(1)) < 2 or int(m.group(2)) != threads:
            print(p.stdout[-2000:])
            print(f"FAIL: expected a multi-variant portfolio on {threads} thread(s)")
            return 1
        s = json.loads(js.read_text())
        outs[threads] = (out.read_bytes(), s["variant"], s["routed"], s["vias"])
        print(f"threads {threads}: {m.group(1)} variants, winner {s['variant']}, routed {s['routed']}/{s['connections']}, vias {s['vias']}, "
              f"{len(outs[threads][0])} bytes")
    ref = outs[1]
    bad = [t for t, o in outs.items() if o != ref]
    if bad:
        print(f"FAIL: output differs from --threads 1 at --threads {bad}")
        return 1
    print("PASS: identical boards at --threads 1, 2, 4")
    return 0


if __name__ == "__main__":
    sys.exit(main())
