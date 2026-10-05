#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Quality benchmark: TraceMaker vs Freerouting versions on held-out PCBench boards, judged identically.

  bench/quality_bench.py --per-tier 20 --tiers A B C --jobs 6 --name quality-1

Boards are sampled (fixed seed) from PCBench boards that never appeared in any bench/results run, so nothing in
the sample was used while developing TraceMaker. Each board goes through bench/compare.py (TraceMaker, Freerouting
2.5.0-RC12 and 1.9.0 with Freerouting's published settings, the human original as reference). Results go to
bench/results/<name>/ (quality.jsonl, summary.json, report.md).
"""
import argparse
import concurrent.futures as cf
import glob
import importlib.util
import json
import pathlib
import random
import statistics
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
_spec = importlib.util.spec_from_file_location("bench_run", ROOT / "bench/run.py")
bench_run = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bench_run)


def used_boards() -> set:
    used = set()
    for f in glob.glob(str(ROOT / "bench/results/*/boards.jsonl")):
        for line in open(f):
            try:
                used.add(json.loads(line)["board"])
            except (ValueError, KeyError):
                pass
    return used


def one(board: str, out: pathlib.Path, fr: list, fr_timeout: str, reuse: str | None = None, fixtures: str | None = None,
        no_tm: bool = False) -> dict:
    t0 = time.time()
    cmd = ["python3", str(ROOT / "bench/compare.py"), board, "--out", str(out / "boards"), "--fr", *([] if reuse else fr),
           "--fr-timeout", fr_timeout]
    if fixtures:
        cmd += ["--fixtures", fixtures]
    if no_tm:
        cmd.append("--no-tm")
    p = subprocess.run(cmd, capture_output=True, text=True)
    q = out / "boards" / board / "quality.json"
    if not q.exists():
        return {"board": board, "error": (p.stderr or p.stdout)[-400:]}
    d = json.loads(q.read_text())
    if reuse:  # Freerouting results do not change between TraceMaker versions: take them from an earlier run
        old = ROOT / "bench/results" / reuse / "boards" / board / "quality.json"
        if old.exists():
            d["runs"] += [r for r in json.loads(old.read_text())["runs"] if r["label"].startswith("Freerouting")]
            q.write_text(json.dumps(d, indent=1))
    d["wall_total_s"] = round(time.time() - t0, 1)
    return d


def summarise(rows: list, labels: list) -> dict:
    s = {}
    for lab in labels:
        rs = [next((r for r in row["runs"] if r["label"] == lab), None) for row in rows if "runs" in row]
        rs = [r for r in rs if r and "completion" in r]
        if not rs:
            continue
        s[lab] = {
            "boards": len(rs),
            "kicad_clean": round(sum(1 for r in rs if r.get("clean")) / len(rs), 4),
            "complete": round(sum(1 for r in rs if r.get("completion") == 1.0) / len(rs), 4),
            "mean_completion": round(statistics.mean(r["completion"] for r in rs), 4),
            "boards_with_router_errors": sum(1 for r in rs if r.get("router_errors")),
            "median_wall_s": statistics.median(r["wall_s"] for r in rs if r.get("wall_s") is not None) if any(r.get("wall_s") for r in rs) else None,
        }
    # Paired quality on boards that both TraceMaker and the other router completed (ratios TraceMaker / other).
    def paired(other):
        ratios = {"length_mm": [], "vias": [], "bends": []}
        sharp = {"TraceMaker": 0, other: 0}
        n = 0
        for row in rows:
            if "runs" not in row:
                continue
            a = next((r for r in row["runs"] if r["label"] == "TraceMaker"), None)
            b = next((r for r in row["runs"] if r["label"] == other), None)
            if not a or not b or a.get("completion") != 1.0 or b.get("completion") != 1.0:
                continue
            n += 1
            for k in ratios:
                if b.get(k):
                    ratios[k].append(a[k] / b[k])
            sharp["TraceMaker"] += a.get("sharp_bends", 0)
            sharp[other] += b.get("sharp_bends", 0)
        return {"boards_both_complete": n, **{f"median_ratio_{k}": round(statistics.median(v), 3) if v else None for k, v in ratios.items()},
                "sharp_bends_total": sharp}
    s["paired"] = {lab: paired(lab) for lab in labels if lab not in ("TraceMaker", "Human original")}
    return s


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--tiers", nargs="*", default=["A", "B", "C"])
    ap.add_argument("--per-tier", type=int, default=20)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--fr", nargs="*", default=["2.5.0-RC12", "1.9.0"])
    ap.add_argument("--fr-timeout", default="00:30:00")
    ap.add_argument("--name", default="quality-1")
    ap.add_argument("--reuse-fr", help="take Freerouting results from this earlier quality run (same boards)")
    ap.add_argument("--dataset", default="pcbench", choices=["pcbench", "dac2020"])
    ap.add_argument("--exclude-run", nargs="*", default=[], help="also exclude boards used by these quality runs")
    ap.add_argument("--no-tm", action="store_true", help="Freerouting only (merge TraceMaker later with --reuse-fr)")
    a = ap.parse_args()
    base = bench_run.fr_baseline()
    used = used_boards()
    rng = random.Random(a.seed)
    boards = []
    for t in a.tiers:
        pool = sorted(n for n, v in base.items() if v.get("tier") == t and n not in used
                      and (bench_run.FIX / n / "unrouted.kicad_pcb").exists() and (bench_run.FIX / n / "unrouted.dsn").exists())
        boards += [(t, n) for n in rng.sample(pool, min(a.per_tier, len(pool)))]
    fixtures = None
    if a.dataset == "dac2020":
        fixtures = str(ROOT / "bench/data/dac2020_prepared")
        boards = [("DAC", d.name) for d in sorted(pathlib.Path(fixtures).glob("bm*"))]
    for run in a.exclude_run:
        seen = {json.loads(line)["board"] for line in open(ROOT / "bench/results" / run / "quality.jsonl")}
        boards = [(t, n) for t, n in boards if n not in seen]
        if a.dataset == "pcbench":  # top the sample back up from the remaining pool
            for t in a.tiers:
                have = sum(1 for tt, _ in boards if tt == t)
                pool = sorted(n for n, v in base.items() if v.get("tier") == t and n not in used and n not in seen
                              and n not in {b for _, b in boards}
                              and (bench_run.FIX / n / "unrouted.kicad_pcb").exists() and (bench_run.FIX / n / "unrouted.dsn").exists())
                boards += [(t, n) for n in rng.sample(pool, min(a.per_tier - have, len(pool)))]
    if a.reuse_fr:  # the same boards as the reused run (the held-out pool shrinks once runs exist)
        boards = [(json.loads(line)["tier"], json.loads(line)["board"]) for line in
                  open(ROOT / "bench/results" / a.reuse_fr / "quality.jsonl")]
    out = ROOT / "bench/results" / a.name
    out.mkdir(parents=True, exist_ok=True)
    print(f"{a.name}: {len(boards)} held-out boards, jobs {a.jobs}", flush=True)
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex, open(out / "quality.jsonl", "w") as log:
        futs = {ex.submit(one, n, out, a.fr, a.fr_timeout, a.reuse_fr, fixtures, a.no_tm): (t, n) for t, n in boards}
        for f in cf.as_completed(futs):
            t, n = futs[f]
            r = f.result()
            r["tier"] = t
            rows.append(r)
            log.write(json.dumps(r) + "\n")
            log.flush()
            brief = {x["label"]: (x.get("completion"), x.get("clean")) for x in r.get("runs", [])}
            print(f"[{len(rows)}/{len(boards)}] {t} {n}: {brief if brief else r.get('error')}", flush=True)
    labels = ["TraceMaker"] + [f"Freerouting {v}" for v in a.fr] + ["Human original"]
    tiers = sorted({t for t, _ in boards})
    summ = {"run": a.name, "set": "DAC 2020 benchmarks" if a.dataset == "dac2020" else "PCBench held-out quality",
            "boards": len(rows), "tiers": tiers, "by_router": summarise(rows, labels),
            "by_tier": {t: summarise([r for r in rows if r.get("tier") == t], labels) for t in tiers}}
    (out / "quality_summary.json").write_text(json.dumps(summ, indent=1))
    print(json.dumps(summ["by_router"], indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
