#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Score a bench/run.py run of a Simple Route JSON set (srj19, srj20) the way tscircuit scores its own router
(doc 10 §7), next to KiCad's verdict that run.py already recorded.

  bench/srj_score.py RUN [RUN ...] [--fixtures DIR] [--no-relaxed]

For every board of bench/results/RUN: `tracemaker srj-export` writes the routed copper back into the problem
(boards/<name>.srj.json), and tscircuit's own evaluateRelaxedDrc (tscircuit-autorouter, lib/testing; 0.1 mm trace
and via clearance) checks it. That needs `scripts/fetch_fixtures.sh tscircuit-checker` and node; without them the
relaxed score is left out and the script says so.

Per run: mean completion, share of boards fully routed, clean pass (fully routed and no added KiCad DRC error),
relaxed pass (fully routed and no error from tscircuit's checker), median and 95th-percentile route time.
Writes bench/results/RUN/srj_score.json and adds `fully_routed` and `relaxed_pass` to summary.json.
"""
import argparse
import concurrent.futures as cf
import json
import math
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
CHECKER = ROOT / "bench/data/tscircuit"
# tscircuit's dashboard, its own router at 1x effort, read on 2026-10-10: share of boards completed and share
# passing its relaxed DRC.
PUBLISHED = {"srj19": {"completed": 0.915, "relaxed_pass": 0.895}, "srj20": {"completed": 0.77, "relaxed_pass": 0.77}}


def fixtures_of(set_name: str) -> pathlib.Path | None:
    f = ROOT / "bench/sets" / f"{set_name}.txt"
    if f.exists():
        for line in f.read_text().splitlines():
            if line.startswith("#fixtures:"):
                return ROOT / line.split(":", 1)[1].strip()
    return None


def percentile(values: list[float], q: float) -> float | None:
    """Nearest-rank percentile."""
    if not values:
        return None
    v = sorted(values)
    return v[max(0, math.ceil(q * len(v)) - 1)]


def relaxed_check(items: list[dict], work: pathlib.Path) -> dict[str, dict] | None:
    """tscircuit's checker on every solved file; None if it is not installed."""
    auto = (CHECKER / "tscircuit-autorouter").resolve()
    tsx = CHECKER.resolve() / "node_modules/.bin/tsx"
    if not (auto / "lib/testing/evaluate-relaxed-drc.ts").exists() or not tsx.exists():
        return None
    lst, out = work / "srj_relaxed_in.json", work / "srj_relaxed_out.jsonl"
    lst.write_text(json.dumps(items))
    out.unlink(missing_ok=True)
    p = subprocess.run([str(tsx), "--tsconfig", str(auto / "tsconfig.json"), str(ROOT / "bench/srj_relaxed_check.mts"), str(auto), str(lst), str(out)],
                       capture_output=True, text=True)
    if p.returncode != 0 or not out.exists():
        print("relaxed check failed to run:", (p.stderr or p.stdout)[-400:], file=sys.stderr)
        return None
    return {r["name"]: r for r in map(json.loads, out.read_text().splitlines())}


def score(run: str, fixtures: pathlib.Path | None, relaxed: bool, jobs: int) -> dict | None:
    rdir = ROOT / "bench/results" / run
    summary = json.loads((rdir / "summary.json").read_text())
    set_name = summary["set"].split()[0]
    fix = fixtures or fixtures_of(set_name)
    if fix is None:
        print(f"{run}: set '{set_name}' names no fixtures directory; give --fixtures")
        return None
    # The export is post-processing: the current build writes it (the run's snapshot only if there is no build).
    tm = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))
    if not tm.exists():
        tm = rdir / "tracemaker"
    rows = [json.loads(x) for x in (rdir / "boards.jsonl").read_text().splitlines()]

    def export(r: dict) -> None:
        board = rdir / "boards" / f"{r['board']}.kicad_pcb"
        solved = rdir / "boards" / f"{r['board']}.srj.json"
        ej = rdir / "boards" / f"{r['board']}.srj-export.json"
        imp = fix / r["board"] / "import.json"
        r["srj_pairs"] = json.loads(imp.read_text())["srj_pairs"] if imp.exists() else None
        if not board.exists():
            return
        p = subprocess.run([str(tm), "srj-export", str(board), "--problem", str(fix / r["board"] / "problem.json"), "-o", str(solved), "--json", str(ej)],
                           capture_output=True, text=True)
        if p.returncode in (0, 3) and ej.exists():
            r["export"] = json.loads(ej.read_text())
            r["solved"] = str(solved)
        else:
            r["export_error"] = (p.stderr or p.stdout).strip()[-300:]

    with cf.ThreadPoolExecutor(jobs) as ex:
        list(ex.map(export, rows))
    checked = relaxed_check([{"name": r["board"], "solved": r["solved"]} for r in rows if "solved" in r], rdir) if relaxed else None
    out_rows = []
    for r in rows:
        e = r.get("export")
        row = {"board": r["board"], "connections": r.get("connections"), "routed": r.get("routed"), "srj_pairs": r.get("srj_pairs"),
               "count_ok": r.get("connections") is not None and r.get("connections") == r.get("srj_pairs"),
               "completion": r.get("completion"), "fully_routed": r.get("unconnected_after") == 0, "clean": bool(r.get("clean")),
               "added_errors": r.get("added_errors"), "seconds": r.get("seconds"),
               "export_routed": e["routed"] if e else None, "export_complete": bool(e) and e["routed"] == e["connections"],
               "extra_traces": e["extra_traces"] if e else None, "export_error": r.get("export_error")}
        if checked is not None:
            c = checked.get(r["board"])
            row["relaxed_errors"] = c.get("errors") if c else None
            row["relaxed_types"] = c.get("types") if c else None
            row["relaxed_failed"] = c.get("failed") if c else "not exported"
            row["relaxed_pass"] = row["export_complete"] and row["relaxed_errors"] == 0
        out_rows.append(row)
    n = len(out_rows)
    secs = [r["seconds"] for r in out_rows if r["seconds"] is not None]
    conns = sum(r["connections"] or 0 for r in out_rows)
    res = {
        "run": run, "set": set_name, "boards": n, "work": summary.get("work"), "route_args": summary.get("route_args"),
        "connections": conns, "connections_routed": sum(r["routed"] or 0 for r in out_rows),
        "count_check_failed": [r["board"] for r in out_rows if not r["count_ok"]],
        "completion": round(sum(r["completion"] or 0 for r in out_rows) / n, 4),
        "fully_routed": round(sum(r["fully_routed"] for r in out_rows) / n, 4),
        "clean_pass": round(sum(r["clean"] for r in out_rows) / n, 4),
        "relaxed_pass": round(sum(bool(r.get("relaxed_pass")) for r in out_rows) / n, 4) if checked is not None else None,
        "relaxed_checker": "tscircuit evaluateRelaxedDrc" if checked is not None else "not run",
        # Boards where KiCad and the export disagree on whether everything is connected (should be none).
        "connectivity_disagreements": [r["board"] for r in out_rows if r["export_routed"] is not None and r["fully_routed"] != r["export_complete"]],
        "export_failures": [r["board"] for r in out_rows if r["export_routed"] is None],
        "checker_failures": [r["board"] for r in out_rows if checked is not None and r.get("relaxed_failed")],
        "median_seconds": percentile(secs, 0.5), "p95_seconds": percentile(secs, 0.95),
        "published_tscircuit": PUBLISHED.get(set_name), "rows": out_rows,
    }
    (rdir / "srj_score.json").write_text(json.dumps(res, indent=1) + "\n")
    summary.update(fully_routed=res["fully_routed"], relaxed_pass=res["relaxed_pass"])
    (rdir / "summary.json").write_text(json.dumps(summary, indent=1))
    return res


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--fixtures", help="fixtures directory (default: the one the run's set file names)")
    ap.add_argument("--no-relaxed", action="store_true", help="skip tscircuit's checker")
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()
    results = [s for s in (score(r, pathlib.Path(a.fixtures).resolve() if a.fixtures else None, not a.no_relaxed, a.jobs) for r in a.runs) if s]
    pct = lambda v: "n/a" if v is None else f"{v:.1%}"  # noqa: E731
    print("| Run | Set | Boards | Work | Completion | Fully routed | Clean pass (KiCad) | Relaxed pass (tscircuit) | Median s | p95 s |")
    print("|---|---|--:|--:|--:|--:|--:|--:|--:|--:|")
    for s in results:
        print(f"| {s['run']} | {s['set']} | {s['boards']} | {s['work']:,} | {pct(s['completion'])} | {pct(s['fully_routed'])} | {pct(s['clean_pass'])} | "
              f"{pct(s['relaxed_pass'])} | {s['median_seconds']:.2f} | {s['p95_seconds']:.2f} |")
    for s in results:
        pub = s["published_tscircuit"]
        if pub:
            print(f"{s['set']}: tscircuit's router, own dashboard 2026-10-10: completed {pct(pub['completed'])}, relaxed pass {pct(pub['relaxed_pass'])}")
        for key in ("count_check_failed", "connectivity_disagreements", "export_failures", "checker_failures"):
            if s[key]:
                print(f"{s['run']}: {key}: {', '.join(s[key][:20])}{' ...' if len(s[key]) > 20 else ''}")
        if s["relaxed_pass"] is None:
            print(f"{s['run']}: tscircuit's checker was not run (scripts/fetch_fixtures.sh tscircuit-checker installs it)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
