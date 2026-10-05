#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Fixed-budget A/B harness for router changes on large, hard boards (global router v2, doc 05 §13).

  bench/m6_bench.py [--work 100000000] [--jobs 8] [--boards ...] -- CONFIG_NAME=ROUTE_ARGS [CONFIG_NAME=ROUTE_ARGS ...]
  e.g. bench/m6_bench.py -- base= global=--global

Each board is routed by one portfolio variant (--variants 1) with a deterministic work budget, so results are
repeatable and independent of machine load; reports routed connections per board and in total, plus seconds.
Not a replacement for the KiCad-judged tiers (bench/run.py), which a change must still pass.
"""
import argparse
import concurrent.futures as cf
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = ROOT / "build/release/src/app/tracemaker"
BOARDS = ["oskirby_logicbone", "decelerator4030_decelerator4030", "kitspace_EEZ%20DIB%20MCU%20r1B2", "sbc_sbc",
          "front-end-modules_LimeSDR_Sony", "Aleste-520EX_aleste", "CoreOne-xCORE200-Original_CoreOne", "LeeChee_1800",
          "MonApollo_analog-board", "KiCad-Library_Teensy_test_layout"]


def one(board: str, cfg: str, args: list[str], work: int, out: pathlib.Path) -> tuple[str, str, int, int, float]:
    o = out / cfg / f"{board}.kicad_pcb"
    o.parent.mkdir(parents=True, exist_ok=True)
    p = subprocess.run([str(TM), "route", str(FIX / board / "unrouted.kicad_pcb"), "-o", str(o), "--work", str(work), "--time", "3600",
                        "--threads", "1", "--variants", "1", "--no-kb"] + args, capture_output=True, text=True)
    m = re.search(r"routed (\d+)/(\d+) connections.*?([\d.]+) s", p.stdout + p.stderr)
    return (board, cfg, int(m.group(1)), int(m.group(2)), float(m.group(3))) if m else (board, cfg, -1, -1, 0.0)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--work", type=int, default=100_000_000)
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--boards", nargs="*", default=BOARDS)
    ap.add_argument("--out", default=str(ROOT / "build/m6"))
    ap.add_argument("configs", nargs="+", help="NAME=ARGS (args space-separated)")
    a = ap.parse_args()
    cfgs = [(c.split("=", 1)[0], c.split("=", 1)[1].split() if "=" in c else []) for c in a.configs]
    res = {}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(one, b, n, args, a.work, pathlib.Path(a.out)) for b in a.boards for n, args in cfgs]
        for f in cf.as_completed(futs):
            b, n, r, t, s = f.result()
            res[(b, n)] = (r, t, s)
    names = [n for n, _ in cfgs]
    print(f"{'board':40} " + " ".join(f"{n:>14}" for n in names))
    tot = {n: 0 for n in names}
    for b in a.boards:
        row = []
        for n in names:
            r, t, s = res[(b, n)]
            tot[n] += max(r, 0)
            row.append(f"{r:>5}/{t:<5} {s:>4.0f}s")
        print(f"{b[:40]:40} " + " ".join(f"{x:>14}" for x in row))
    print(f"{'total routed':40} " + " ".join(f"{tot[n]:>14}" for n in names))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
