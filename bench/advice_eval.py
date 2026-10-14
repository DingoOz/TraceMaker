#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Measure advice (doc 17 §7): route boards with `route --advisor CMD`, which routes plainly, consults the advisor only when
connections are left, and routes again with its advice. Per board: connections routed by the plain run, by the advised run,
and whether the advised result was kept. The gate of issue #26 stage 2: advice routes more on boards that were not used to tune
the advisor (the odd half of the set), and never fewer anywhere (it cannot be, with a work budget: the plain run's result is kept
unless the advised one is better).

  bench/advice_eval.py --set srj20-m15fail4 --name adv-a --work 20000000 --variants 8 --threads 8 --jobs 4 \
      --advisor "python3 advisors/heuristic_advisor.py" [--half even|odd|all] [--route-args "..."]
  bench/advice_eval.py ... --control --name adv-a-control      # the same boards, no advice, V + 4 variants: equal compute

--time is 3600 s on purpose: with the default 120 s a loaded machine ends variants early, the work budget stops being the only stop, and the
same command gives different counts (seen on 2026-10-14: the plain run of one board gave 54, 50 and 54 in three experiments).

Writes bench/results/<name>/advice.json (one row per board, and a summary) and the advice files it used.
"""
import argparse
import concurrent.futures as cf
import json
import os
import pathlib
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))


def read_set(name):
    fixtures, boards = None, []
    for line in (ROOT / "bench/sets" / f"{name}.txt").read_text().splitlines():
        if line.startswith("#fixtures:"):
            fixtures = ROOT / line.split(":", 1)[1].strip()
        elif line.strip() and not line.startswith("#"):
            boards.append(line.strip())
    return fixtures, boards


def run_board(job):
    board_dir, name, out, a = job
    d = out / "boards"
    d.mkdir(parents=True, exist_ok=True)
    js, advice, report = d / f"{name}.control.json" if a.control else d / f"{name}.json", d / f"{name}.advice.json", d / f"{name}.stuck.json"
    if a.control:
        # The equal-compute control: no advice, the variants the advised run would have had (the plain ones and the advised copies), so that
        # any gain of advice over this is not just more variants.
        cmd = [str(TM), "route", str(board_dir / name / "unrouted.kicad_pcb"), "-o", str(d / f"{name}.control.kicad_pcb"), "--work", str(a.work),
               "--variants", str(a.variants + a.advice_variants), "--threads", str(a.threads), "--no-kb", "--time", "3600", "--json", str(js)] + a.route_args.split()
    else:
        cmd = [str(TM), "route", str(board_dir / name / "unrouted.kicad_pcb"), "-o", str(d / f"{name}.kicad_pcb"), "--work", str(a.work),
               "--variants", str(a.variants), "--threads", str(a.threads), "--no-kb", "--time", "3600", "--json", str(js), "--stuck-report", str(report),
               "--advice-out", str(advice), "--advisor-timeout", "60"]
        if a.advisor:
            cmd += ["--advisor", a.advisor]
        cmd += a.route_args.split()
    t0 = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    row = {"board": name, "seconds": round(time.time() - t0, 1), "exit": p.returncode}
    if not js.exists():
        row["error"] = (p.stderr or p.stdout)[-400:]
        return row
    j = json.loads(js.read_text())
    row["connections"] = j["connections"]
    row["routed"] = j["routed"]
    rounds = (j.get("advice") or {}).get("advisor", {}).get("rounds", [])
    if rounds:
        r = rounds[0]
        row["plain"] = r["routed_before"]
        row["advised"] = r.get("routed_after")
        row["adopted"] = r.get("adopted", False)
        row["advised_variants_best"] = r.get("advised_best_routed")
        row["error_advisor"] = r.get("error")
    else:
        row["plain"] = j["routed"]  # complete already (or the control): the advisor was not consulted
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--work", type=int, default=20_000_000)
    ap.add_argument("--variants", type=int, default=8)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--advisor", default="")
    ap.add_argument("--half", choices=["even", "odd", "all"], default="all")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--route-args", default="")
    ap.add_argument("--control", action="store_true", help="the equal-compute control: plain route with --variants V + --advice-variants")
    ap.add_argument("--advice-variants", type=int, default=4, help="the advice's variants (default 4), for the control")
    a = ap.parse_args()
    fixtures, boards = read_set(a.set)
    boards = [b for i, b in enumerate(boards) if a.half == "all" or (i % 2 == 0) == (a.half == "even")]
    if a.limit:
        boards = boards[: a.limit]
    out = ROOT / "bench/results" / a.name
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for row in ex.map(run_board, [(fixtures, b, out, a) for b in boards]):
            rows.append(row)
            print(f"{row['board']:12} plain {row.get('plain', '-'):>4} advised {str(row.get('advised', '-')):>4} final {row.get('routed', '-'):>4}/{row.get('connections', '-')}"
                  f"{'  ADOPTED' if row.get('adopted') else ''}{'  ' + str(row.get('error_advisor')) if row.get('error_advisor') else ''}  {row['seconds']} s", flush=True)
    done = [r for r in rows if "routed" in r]
    s = {"set": a.set, "half": a.half, "boards": len(rows), "work": a.work, "variants": a.variants, "advisor": a.advisor,
         "plain_routed": sum(r["plain"] for r in done), "final_routed": sum(r["routed"] for r in done),
         "better": sum(1 for r in done if r["routed"] > r["plain"]), "worse": sum(1 for r in done if r["routed"] < r["plain"]),
         "complete_plain": sum(1 for r in done if r["plain"] == r["connections"]),
         "complete_final": sum(1 for r in done if r["routed"] == r["connections"]), "advisor_failed": sum(1 for r in rows if r.get("error_advisor"))}
    (out / "advice.json").write_text(json.dumps({"summary": s, "rows": rows}, indent=1))
    print(json.dumps(s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
