#!/usr/bin/env python3
"""Differential-pair evaluation (M12, doc 05 §14): routes boards with and without coupled pair routing at a fixed
deterministic budget and reports, per pair, the coupled share of its length, the gap kept and the intra-pair skew
(`tracemaker pairs`), and per board the completion and KiCad DRC errors added by routing (kicad-cli, Docker).

  bench/pair_eval.py BOARD [BOARD ...] [--work 30000000] [--jobs 4] [--drc-jobs 2] [--out build/pair_eval]
                     [--on-args "--diff-pairs"] [--json results.json]

BOARD is a PCBench fixture name or a .kicad_pcb path. Both runs use --threads 1 --no-kb --no-gpu, so they are
deterministic and differ only by the pair options.
"""
import argparse
import concurrent.futures as cf
import importlib.util
import json
import pathlib
import shlex
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = ROOT / "build/release/src/app/tracemaker"
_spec = importlib.util.spec_from_file_location("bench_run", ROOT / "bench/run.py")
bench_run = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bench_run)
FIX_DIRS = [ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench",
            pathlib.Path.home() / "Programming/TraceMaker/bench/data/freerouting/scripts/benchmark/fixtures/PCBench"]


def board_path(name: str) -> pathlib.Path:
    p = pathlib.Path(name)
    if p.suffix == ".kicad_pcb":
        return p
    for d in FIX_DIRS:
        if (d / name / "unrouted.kicad_pcb").exists():
            return d / name / "unrouted.kicad_pcb"
    raise SystemExit(f"no board {name}")


def route(src: pathlib.Path, out: pathlib.Path, work: int, extra: list[str]) -> dict:
    js = out.with_suffix(".json")
    if not out.exists() or not js.exists():
        cmd = [str(TM), "route", str(src), "-o", str(out), "--work", str(work), "--threads", "1", "--no-kb", "--no-gpu",
               "--json", str(js), *extra]
        r = subprocess.run(cmd, capture_output=True, text=True)
        (out.with_suffix(".log")).write_text(r.stderr + r.stdout)
        if not js.exists():  # the router exits non-zero when connections stay unrouted; only a missing summary is an error
            return {"error": r.returncode}
    return json.loads(js.read_text())


def pairs(board: pathlib.Path, extra_pairs: list[str]) -> list[dict]:
    js = board.with_suffix(".pairs.json")
    cmd = [str(TM), "pairs", str(board), "--json", str(js)]
    for p in extra_pairs:
        cmd += ["--pair", p]
    subprocess.run(cmd, capture_output=True, check=True)
    return json.loads(js.read_text())["pairs"]


def added_errors(before: dict, after: dict) -> dict:
    return {t: n - before["routed_errors"].get(t, 0) for t, n in after["routed_errors"].items()
            if t not in bench_run.NOT_ROUTING and n - before["routed_errors"].get(t, 0) > 0}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--work", type=int, default=30_000_000)
    ap.add_argument("--jobs", type=int, default=4, help="router processes at once (each single-threaded)")
    ap.add_argument("--drc-jobs", type=int, default=2, help="kicad-cli containers at once")
    ap.add_argument("--out", default=str(ROOT / "build/pair_eval"))
    ap.add_argument("--on-args", default="--diff-pairs")
    ap.add_argument("--pair", action="append", default=[], help="BOARD:NET_A,NET_B pairs not named P/N or +/-")
    ap.add_argument("--json")
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    extra_pairs: dict[str, list[str]] = {}
    for p in a.pair:
        b, pr = p.split(":", 1)
        extra_pairs.setdefault(b, []).append(pr)
    on_args = shlex.split(a.on_args)
    jobs = []
    for name in a.boards:
        src = board_path(name)
        tag = pathlib.Path(name).stem if name.endswith(".kicad_pcb") else name
        for mode, extra in (("off", []), ("on", on_args)):
            jobs.append((tag, mode, src, out / f"{tag}.{mode}.kicad_pcb", extra))
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        res = dict(zip([(t, m) for t, m, *_ in jobs], ex.map(lambda j: route(j[2], j[3], a.work, j[4]), jobs)))
    drc_files = sorted({j[2] for j in jobs}) + [j[3] for j in jobs]
    with cf.ThreadPoolExecutor(a.drc_jobs) as ex:
        drcs = dict(zip(drc_files, ex.map(bench_run.drc, drc_files)))
    rows = []
    for tag, mode, src, dst, _ in jobs:
        r = res[(tag, mode)]
        before, after = drcs.get(src), drcs.get(dst)
        row = {"board": tag, "mode": mode, "routed": r.get("routed"), "connections": r.get("connections")}
        if before and after:
            row["unconnected"] = after["unconnected"]
            row["added_errors"] = added_errors(before, after)
            row["clean"] = after["unconnected"] == 0 and not row["added_errors"]
        row["pairs"] = pairs(dst, extra_pairs.get(tag, [])) if dst.exists() else []
        rows.append(row)
    print(f"{'board':34} {'mode':4} {'routed':>9} {'uncon':>5} {'added':>5}  pairs: coupled% gap(median/min) skew")
    for r in rows:
        pr = "  ".join(f"{p['net_a']}:{100 * p['coupled_share']:.0f}% {p['gap_median_mm']:.3f}/{p['gap_min_mm']:.3f} {p['skew_mm']:.2f}"
                       for p in r["pairs"])
        print(f"{r['board'][:34]:34} {r['mode']:4} {r['routed']}/{r['connections']:<4} {r.get('unconnected', '?'):>5} "
              f"{sum(r.get('added_errors', {}).values()) if 'added_errors' in r else '?':>5}  {pr}")
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps({"work": a.work, "on_args": a.on_args, "rows": rows}, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
