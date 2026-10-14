#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Route the fanout31 cases (bench/prepare_fanout31.py) and score them as tscircuit does: a case is solved only when
every connection of the problem is routed (the router's own count after refilling the plane zones: no unconnected
item). Writes bench/results/<name>/fanout31.json and prints a per-family table.

  bench/fanout31_run.py --name fan31-a [--work 60000000] [--jobs 12] [--variants 1] [--route-args "..."]
"""
import argparse, concurrent.futures as cf, json, os, pathlib, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))
BOARDS = ROOT / "bench/data/tscircuit/boards/fanout31"
FAMILIES = ["am62l", "rk3308", "k230", "imx6ull", "t113s3", "am3352"]


def family(case: str) -> str:
    for f in FAMILIES[1:]:
        if f in case:
            return f
    return "am62l"


def run(job):
    case, out, work, variants, threads, extra, limit = job
    d = out / "boards"
    d.mkdir(parents=True, exist_ok=True)
    js = d / f"{case}.route.json"
    cmd = [str(TM), "route", str(BOARDS / case / "unrouted.kicad_pcb"), "-o", str(d / f"{case}.kicad_pcb"), "--work", str(work),
           "--variants", str(variants), "--threads", str(threads), "--no-kb", "--soft-zones", "--pitch-um", "40", "--time", str(limit),
           "--json", str(js)] + extra
    t = time.time()
    p = subprocess.run(cmd, capture_output=True, text=True)
    row = {"case": case, "family": family(case), "seconds": time.time() - t, "exit": p.returncode}
    try:
        r = json.loads(js.read_text())
        row.update(routed=r["routed"], connections=r["connections"], vias=r["vias"], open=r.get("unconnected_after_refill", r["connections"] - r["routed"]))
        row["solved"] = row["open"] == 0 and r["routed"] == r["connections"] or row["open"] == 0
    except (OSError, ValueError, KeyError):
        row["solved"] = False
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--name", required=True)
    ap.add_argument("--work", type=int, default=60_000_000)
    ap.add_argument("--jobs", type=int, default=12)
    ap.add_argument("--variants", type=int, default=1)
    ap.add_argument("--threads", type=int, default=1)
    ap.add_argument("--time", type=int, default=900)
    ap.add_argument("--only", default="", help="substring filter on case names")
    ap.add_argument("--cases", default="", help="comma-separated case-number prefixes (e.g. 28,29,31)")
    ap.add_argument("--route-args", default="--fast-bends --no-gpu")
    a = ap.parse_args()
    out = ROOT / "bench/results" / a.name
    out.mkdir(parents=True, exist_ok=True)
    want = [c for c in a.cases.split(",") if c]
    cases = sorted(p.name for p in BOARDS.iterdir() if p.is_dir() and a.only in p.name and (not want or p.name.split("-")[0] in want))
    jobs = [(c, out, a.work, a.variants, a.threads, a.route_args.split(), a.time) for c in cases]
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(run, jobs):
            rows.append(r)
            print(f"{'SOLVED' if r['solved'] else 'open  '} {r['case']:36} open {r.get('open', '?')}/{r.get('connections', '?')} {r['seconds']:.0f}s", flush=True)
            (out / "fanout31.json").write_text(json.dumps(rows, indent=1))
    print(f"\n{a.name}: solved {sum(r['solved'] for r in rows)}/{len(rows)}")
    for f in FAMILIES:
        fr = [r for r in rows if r["family"] == f]
        if fr:
            print(f"  {f:8} {sum(r['solved'] for r in fr)}/{len(fr)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
