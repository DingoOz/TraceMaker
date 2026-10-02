#!/usr/bin/env python3
"""TraceMaker benchmark harness (design doc 10).

Routes PCBench fixtures with TraceMaker, judges input and output with `kicad-cli pcb drc`, and compares with
Freerouting's own published per-board results (scripts/benchmark/results/benchmarks.json in the Freerouting
repository, judged by KiCad DRC on Freerouting's side).

  bench/run.py --tier A --limit 60 --time 60 --jobs 12 [--name run-id]

Writes bench/results/<run-id>/{boards.jsonl, summary.json, report.md}; summary.json is shown on the progress site.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import datetime
import hashlib
import json
import pathlib
import random
import subprocess
import sys
import time
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
FR_RESULTS = ROOT / "bench/data/freerouting/scripts/benchmark/results/benchmarks.json"
TM = ROOT / "build/release/src/app/tracemaker"
DRC_CACHE = ROOT / "build/drc/kicad"
# Violation types that routing cannot cause; ignored when counting added errors.
NOT_ROUTING = {"lib_footprint_issues", "lib_footprint_mismatch", "silk_overlap", "silk_over_copper", "silk_edge_clearance",
               "text_height", "text_thickness", "courtyards_overlap", "missing_courtyard", "malformed_courtyard",
               "footprint_type_mismatch", "footprint_filters_mismatch", "nonmirrored_text_on_back_layer",
               "npth_inside_courtyard", "pth_inside_courtyard", "duplicate_footprints", "extra_footprint",
               "missing_footprint", "footprint_symbol_mismatch", "unconnected_items", "track_dangling", "via_dangling",
               "isolated_copper", "starved_thermal", "lib_footprint_mismatch", "holes_co_located"}


def fr_baseline() -> dict[str, dict]:
    """Per-board Freerouting outcome by version label."""
    if not FR_RESULTS.exists():
        return {}
    d = json.loads(FR_RESULTS.read_text())
    out: dict[str, dict] = {}
    for r in d["runs"]:
        fx = r.get("fixture", {})
        if fx.get("group") != "PCBench":
            continue
        name = fx["relative_path"].split("/")[1]
        q = r.get("quality", {})
        ver = r["binary"]["version_label"]
        out.setdefault(name, {"tier": fx.get("tier")})[ver] = {
            "unrouted": q.get("unrouted_connections"), "violations": q.get("clearance_violations"),
            "clean": q.get("unrouted_connections") == 0 and q.get("clearance_violations") == 0,
            "seconds": q.get("wall_clock_seconds"), "timed_out": r.get("exit", {}).get("timed_out")}
    return out


def drc(path: pathlib.Path, timeout: int = 600) -> dict | None:
    DRC_CACHE.mkdir(parents=True, exist_ok=True)
    st = path.stat()
    key = hashlib.sha1(f"{path.resolve()}:{st.st_mtime_ns}:{st.st_size}".encode()).hexdigest()[:16]
    out = DRC_CACHE / f"{key}.json"
    if not out.exists():
        try:
            subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "-o", str(out), str(path)],
                           capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
    if not out.exists():
        return None
    d = json.loads(out.read_text())
    c = Counter(v["type"] for v in d.get("violations", []) if v.get("severity") == "error")
    return {"errors": dict(c), "unconnected": len(d.get("unconnected_items", []))}


def run_board(name: str, outdir: pathlib.Path, time_limit: float) -> dict:
    src = FIX / name / "unrouted.kicad_pcb"
    out = outdir / "boards" / f"{name}.kicad_pcb"
    out.parent.mkdir(parents=True, exist_ok=True)
    res = {"board": name}
    t0 = time.time()
    try:
        p = subprocess.run([str(TM), "route", str(src), "-o", str(out), "--time", str(time_limit), "--json", str(out) + ".route.json"],
                           capture_output=True, text=True, timeout=time_limit * 3 + 60)
        res["exit"] = p.returncode
        if p.returncode not in (0, 3):
            res["error"] = (p.stderr or p.stdout)[-500:]
    except subprocess.TimeoutExpired:
        res["exit"] = -1
        res["error"] = "timeout"
    res["wall_s"] = round(time.time() - t0, 2)
    rj = pathlib.Path(str(out) + ".route.json")
    if rj.exists():
        r = json.loads(rj.read_text())
        res.update({k: r[k] for k in ("routed", "connections", "tracks", "vias", "seconds")})
    before = drc(src)
    after = drc(out) if out.exists() else None
    if before is None or after is None:
        res["judge"] = "failed"
        return res
    added = {t: n - before["errors"].get(t, 0) for t, n in after["errors"].items()
             if t not in NOT_ROUTING and n - before["errors"].get(t, 0) > 0}
    res.update({"unconnected_before": before["unconnected"], "unconnected_after": after["unconnected"], "added_errors": added})
    res["completion"] = 1.0 if before["unconnected"] == 0 else round(1 - after["unconnected"] / before["unconnected"], 4)
    res["clean"] = after["unconnected"] == 0 and not added
    return res


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tier", default="A", help="Freerouting tier: A, B, C, D or all")
    ap.add_argument("--limit", type=int, default=50)
    ap.add_argument("--time", type=float, default=60)
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--seed", type=int, default=1, help="board sampling seed")
    ap.add_argument("--name")
    ap.add_argument("--boards", nargs="*", help="explicit board folder names")
    a = ap.parse_args()
    base = fr_baseline()
    if a.boards:
        names = a.boards
    else:
        names = sorted(n for n, v in base.items() if (a.tier == "all" or v.get("tier") == a.tier) and (FIX / n / "unrouted.kicad_pcb").exists())
        random.Random(a.seed).shuffle(names)
        names = sorted(names[: a.limit])
    run_id = a.name or datetime.datetime.now().strftime("%Y%m%d-%H%M%S") + f"-tier{a.tier}"
    outdir = ROOT / "bench/results" / run_id
    outdir.mkdir(parents=True, exist_ok=True)
    print(f"run {run_id}: {len(names)} boards, tier {a.tier}, {a.time:.0f} s per board, {a.jobs} jobs", flush=True)
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex, (outdir / "boards.jsonl").open("w") as f:
        for r in ex.map(lambda n: run_board(n, outdir, a.time), names):
            fr = base.get(r["board"], {})
            r["fr_rc12"] = fr.get("2.5.0-RC12")
            r["fr_241"] = fr.get("2.4.1")
            r["tier"] = fr.get("tier")
            rows.append(r)
            f.write(json.dumps(r) + "\n")
            f.flush()
            tag = "CLEAN" if r.get("clean") else ("ERR  " if r.get("error") or r.get("judge") else "     ")
            frc = r["fr_rc12"]["clean"] if r.get("fr_rc12") else None
            print(f"{tag} {r['board'][:44]:44} routed {r.get('routed', '?')}/{r.get('connections', '?')} unconn {r.get('unconnected_after', '?')} "
                  f"added {r.get('added_errors', {})} {r.get('seconds', 0):.1f}s  FR-RC12 clean={frc}", flush=True)
    judged = [r for r in rows if "clean" in r]
    n = len(judged)
    clean = sum(r["clean"] for r in judged)
    fr_rows = [r for r in judged if r.get("fr_rc12")]
    fr_clean = sum(r["fr_rc12"]["clean"] for r in fr_rows)
    fr241 = [r for r in judged if r.get("fr_241")]
    both = sum(1 for r in fr_rows if r["clean"] and r["fr_rc12"]["clean"])
    only_tm = sum(1 for r in fr_rows if r["clean"] and not r["fr_rc12"]["clean"])
    only_fr = sum(1 for r in fr_rows if not r["clean"] and r["fr_rc12"]["clean"])
    added_types = Counter()
    for r in judged:
        for t in r["added_errors"]:
            added_types[t] += 1
    summary = {
        "run": run_id, "set": f"PCBench tier {a.tier}", "boards": n,
        "clean_pass": round(clean / n, 4) if n else None,
        "completion": round(sum(r["completion"] for r in judged) / n, 4) if n else None,
        "seconds": round(sum(r.get("seconds", 0) for r in judged), 1),
        "time_limit_s": a.time,
        "fr_rc12_clean_pass": round(fr_clean / len(fr_rows), 4) if fr_rows else None,
        "fr_241_clean_pass": round(sum(r["fr_241"]["clean"] for r in fr241) / len(fr241), 4) if fr241 else None,
        "both_clean": both, "only_tracemaker_clean": only_tm, "only_freerouting_clean": only_fr,
        "boards_with_added_errors": sum(1 for r in judged if r["added_errors"]),
        "added_error_types": dict(added_types),
        "judge_failures": len(rows) - n,
    }
    (outdir / "summary.json").write_text(json.dumps(summary, indent=1))
    lines = [f"# Benchmark {run_id}", "", f"PCBench tier {a.tier}, {n} boards, {a.time:.0f} s limit per board.", "",
             "| Metric | TraceMaker | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |", "|---|--:|--:|--:|",
             f"| Clean pass (KiCad DRC) | {summary['clean_pass']:.1%} | {summary['fr_rc12_clean_pass'] or 0:.1%} | {summary['fr_241_clean_pass'] or 0:.1%} |",
             f"| Mean completion | {summary['completion']:.1%} | | |", "",
             f"Both clean: {both}; only TraceMaker: {only_tm}; only Freerouting: {only_fr}.",
             f"Boards with added DRC errors: {summary['boards_with_added_errors']} ({dict(added_types)})."]
    (outdir / "report.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
