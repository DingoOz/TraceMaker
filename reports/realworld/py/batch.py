#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""batch.py LIST.txt RUN [--jobs N] [--rounds R] [closure options...]: the full stack (prepare, closure loop, KiCad ledger and safety simulation when
KiCad shows no open connection) over many PCBench boards, resumable.

  LIST.txt   one PCBench board name per line (# comments)
  RUN        the closure run name, e.g. closure-b1 (boards/NAME/RUN/); results in reports/realworld/batch/RUN.jsonl and RUN.md

Per board: layers from ground_truth.json, plane layers (inner layers with pours in the designer's file) kept free of signal tracks, then closure.py with a
time limit. A board counts as routed when KiCad shows 0 open connections and the safety simulation does not FAIL.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import pathlib
import re
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import pcbtext
import rules

ROOT = pathlib.Path(__file__).resolve().parents[3]
RW = ROOT / "reports/realworld"
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def prepare(name: str) -> tuple[int, list[str]]:
    unrouted = (FIX / name / "unrouted.kicad_pcb").read_text(errors="replace")
    declared = set(re.findall(r"\(\d+ ((?:F|B|In\d+)\.Cu) ", unrouted))  # the copper layers the file to be routed has (ground_truth.json may say more)
    layers = len(declared) or 2
    raw = (FIX / name / "raw.kicad_pcb").read_text(errors="replace")
    B = RW / "boards" / name
    B.mkdir(parents=True, exist_ok=True)
    (B / "planes.kicad_pcb").write_text(rules.with_designer_zones(unrouted, raw))
    prl = FIX / name / "unrouted.kicad_prl"
    if prl.exists():
        (B / "planes.kicad_prl").write_text(prl.read_text())
    import kgeom
    edge = kgeom.load(str(FIX / name / "unrouted.kicad_pcb")).edge
    xs = [e[0] for e in edge] + [e[2] for e in edge]
    ys = [e[1] for e in edge] + [e[3] for e in edge]
    area = (max(xs) - min(xs)) * (max(ys) - min(ys)) if xs else 1.0
    inner = set(rules.plane_layers(raw, area))  # only real planes are kept free of tracks; a split or partial pour layer is a routing layer
    return layers, sorted(inner & declared)


def run_board(name: str, run: str, rounds: int, extra: list[str], limit_s: int, safety: bool) -> dict:
    t0 = time.time()
    out = {"board": name, "run": run}
    try:
        layers, inner = prepare(name)
        out["layers"] = layers
        args = [sys.executable, str(RW / "py/closure.py"), f"{name}+planes", run, "--rounds", str(rounds), "--move-trials", "3", "--threads", "4", "--trial-jobs", "2", "--deadline-min", str(max(10, limit_s // 60 - 8))] + extra
        if inner:
            args += ["--no-tracks-on", ",".join(inner)]
        log = RW / "boards" / name / f"{run}.log"
        with open(log, "w") as f:
            try:
                subprocess.run(args, stdout=f, stderr=subprocess.STDOUT, timeout=limit_s)
            except subprocess.TimeoutExpired:
                out["timeout"] = True
        cj = RW / "boards" / name / run / "closure.json"
        if cj.exists():
            c = json.loads(cj.read_text())
            rd = [r for r in c["rounds"] if r["kind"] in ("route", "effort", "fine", "window", "escape", "shove") and "open" in r]
            first = next((r for r in c["rounds"] if r["kind"] == "route"), None)
            out["rounds"] = sum(1 for r in c["rounds"] if r["kind"] == "route")
            out["open_first"] = first["open"] if first else None
            out["open_best"] = min((r["open"] for r in rd), default=None)
            out["relaxed"] = len(c["assign"])
            out["relaxed_power"] = sum(1 for n in c["assign"] if n in c["power"])
            out["moves"] = len(c["moves"])
            out["hard"] = len(c.get("hard", []))
        rr = RW / "boards" / name / run / "routed.kicad_pcb"
        if rr.exists() and out.get("open_best") == 0 and safety:
            subprocess.run(["bash", str(RW / "py/judge_run.sh"), name, run, str(layers)], stdout=open(RW / "boards" / name / f"{run}.judge.log", "w"), stderr=subprocess.STDOUT, timeout=1800)
            sj = RW / "sim/results" / name / f"safety.{run}.json"
            lj = RW / "boards" / name / run / "ledger.json"
            if lj.exists():
                led = json.loads(lj.read_text())
                out["kicad_open"] = led["unconnected"]
                out["compromises"] = len(led["added"])
            if sj.exists():
                out["safety"] = json.loads(sj.read_text()).get("verdict")
    except Exception as e:  # a board that breaks the pipeline is a result too
        out["error"] = f"{type(e).__name__}: {e}"
    out["seconds"] = round(time.time() - t0)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("list")
    ap.add_argument("run")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--rounds", type=int, default=6)
    ap.add_argument("--limit-min", type=int, default=40, help="time limit per board, minutes")
    ap.add_argument("--no-safety", action="store_true")
    a, extra = ap.parse_known_args()
    names = [l.strip() for l in pathlib.Path(a.list).read_text().splitlines() if l.strip() and not l.startswith("#")]
    res_path = RW / "batch" / f"{a.run}.jsonl"
    res_path.parent.mkdir(exist_ok=True)
    done = {json.loads(l)["board"] for l in res_path.read_text().splitlines()} if res_path.exists() else set()
    todo = [n for n in names if n not in done]
    print(f"{len(names)} boards, {len(done)} done, {len(todo)} to do, {a.jobs} at a time", flush=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex, open(res_path, "a") as fh:
        futs = {ex.submit(run_board, n, a.run, a.rounds, extra, a.limit_min * 60, not a.no_safety): n for n in todo}
        for f in cf.as_completed(futs):
            r = f.result()
            fh.write(json.dumps(r) + "\n")
            fh.flush()
            print(r, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
