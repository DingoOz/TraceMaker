#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Freerouting 2.1.0 with the PCBWorld paper's settings on the D3 test boards, judged by this repository's KiCad judge (M17, doc 18 A1).

  bench/pcbworld_fr.py --name m17-pcbworld-fr21 [--set pcbworld-d3-test] [--jobs 8] [--timeout 600] [--runs 1]

The paper (arXiv 2607.05915, Table 20) runs freerouting-2.1.0.jar with `-mp 10` (ten passes) and a 4 GiB heap, everything else at the jar's
defaults. This does the same from the Specctra DSN of each PCBench fixture, imports the session into the fixture's board
(bench/ses_import.py) and judges it exactly as bench/run.py judges TraceMaker (errors added against the input, connectivity), so the two
columns share one judge. The jar is downloaded by hand: bench/data/freerouting/scripts/benchmark/binaries/freerouting-2.1.0.jar from
https://github.com/freerouting/freerouting/releases/tag/v2.1.0 (sha256 2c07d58f75dac03782664081e7a58b41c25400d871a9fcf166a2ea6fe60d5def).
Output: bench/results/NAME/boards.jsonl (one row per board and run: clean, completion, router_errors, seconds), summary.json.
"""
from __future__ import annotations

import argparse
import concurrent.futures
import importlib.util
import json
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FRB = ROOT / "bench/data/freerouting/scripts/benchmark"
FIX = FRB / "fixtures/PCBench"
JAR = FRB / "binaries/freerouting-2.1.0.jar"
JAVA = next(iter(sorted((ROOT / "build/tools").glob("jdk-25*/bin/java"))), pathlib.Path("java"))


def load(path: pathlib.Path, name: str):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    sys.modules[name] = mod
    spec.loader.exec_module(mod)
    return mod


def one(quality, out: pathlib.Path, board: str, run: int, timeout: int) -> dict:
    src = FIX / board
    d = out / board / f"run{run}"
    d.mkdir(parents=True, exist_ok=True)
    ses, pcb = d / "fr.ses", d / "fr.kicad_pcb"
    cmd = [str(JAVA), "-Xmx4g", "-Djava.awt.headless=true", "-jar", str(JAR), "-de", str(src / "unrouted.dsn"), "-do", str(ses), "-mp", "10"]
    t0 = time.time()
    row = {"board": board, "run": run}
    try:
        with open(d / "fr.log", "w") as f:
            p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT, timeout=timeout)
        row["seconds"] = round(time.time() - t0, 1)
        if p.returncode != 0 or not ses.exists():
            row.update(error=f"freerouting exit {p.returncode}", clean=False, completion=0.0)
            return row
    except subprocess.TimeoutExpired:
        row.update(seconds=round(time.time() - t0, 1), error="timeout", clean=False, completion=0.0)
        return row
    subprocess.run([sys.executable, str(ROOT / "bench/ses_import.py"), str(src / "unrouted.kicad_pcb"), str(ses), "-o", str(pcb)], check=True, capture_output=True)
    m = quality.metrics(src / "unrouted.kicad_pcb", pcb)
    row.update({k: m.get(k) for k in ("clean", "completion", "router_errors", "unconnected", "vias", "length_mm")})
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True)
    ap.add_argument("--set", default="pcbworld-d3-test")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=600, help="seconds per run (the paper's mean is 7 s on D3-A, 10 s on D3-B)")
    ap.add_argument("--runs", type=int, default=1)
    a = ap.parse_args()
    if not JAR.exists():
        print(f"SKIP: {JAR} is missing (see --help)")
        return 77
    quality = load(ROOT / "bench/quality.py", "quality")
    boards = [x.strip() for x in (ROOT / "bench/sets" / f"{a.set}.txt").read_text().splitlines() if x.strip() and not x.startswith("#")]
    out = ROOT / "bench/results" / a.name
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    with (out / "boards.jsonl").open("w") as f, concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        futs = [ex.submit(one, quality, out, b, r, a.timeout) for b in boards for r in range(a.runs)]
        for fut in concurrent.futures.as_completed(futs):
            row = fut.result()
            rows.append(row)
            f.write(json.dumps(row) + "\n")
            f.flush()
            print(f"{'CLEAN' if row.get('clean') else '     '} {row['board']:50s} completion {row.get('completion')} errors {row.get('router_errors')} {row.get('seconds')}s {row.get('error', '')}", flush=True)
    n = len(rows)
    (out / "summary.json").write_text(json.dumps({"set": a.set, "boards": len(boards), "runs": n, "clean_pass": sum(1 for r in rows if r.get("clean")) / n,
                                                   "completion": sum(r.get("completion") or 0 for r in rows) / n,
                                                   "seconds": sum(r.get("seconds") or 0 for r in rows)}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
