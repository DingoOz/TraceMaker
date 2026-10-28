#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Why connections stay open, over a benchmark run's routed boards (doc 05 §55, M20 C5).

For each board of a finished `bench/run.py` run, `tracemaker causes` classifies every open connection by the first change of the copper that
lets the router route it (through traffic near its ends, the package's own fan-out near its ends, the channel away from them, both, none), and
a copy of the routed board with two more inner layers (`tracemaker layers`) is routed as it is to count what more layers alone would close.

  bench/failure_causes.py --run c3-bd-base --name c5-causes [--jobs 6 --threads 8 --work 2000000 --radius 2]

Writes bench/results/<name>/{causes.json, causes.md} and one <board>.causes.json per board.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))
CAUSES = ["pin_cannot_escape", "through_traffic", "package_fanout", "channel", "near_and_far", "no_path_bare"]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--run", required=True, help="a finished run in bench/results/<RUN> (its boards/*.kicad_pcb are the routed boards)")
    ap.add_argument("--results", default=str(ROOT / "bench/results"), help="where the run is and the output goes")
    ap.add_argument("--name", required=True)
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--work", type=int, default=2_000_000)
    ap.add_argument("--baseline-work", type=int, default=10_000_000)
    ap.add_argument("--radius", type=float, default=2.0)
    ap.add_argument("--extra-layers", type=int, default=2, help="inner layers added for the layer test (0 = no layer test)")
    ap.add_argument("--fixtures", default=str(FIX))
    a = ap.parse_args()
    res = pathlib.Path(a.results)
    rows = [json.loads(x) for x in (res / a.run / "boards.jsonl").read_text().splitlines() if x.strip()]
    out = res / a.name
    scratch = ROOT / "build/scratch/causes" / a.name
    out.mkdir(parents=True, exist_ok=True)
    scratch.mkdir(parents=True, exist_ok=True)

    def one(r: dict) -> dict:
        b = r["board"]
        routed = res / a.run / "boards" / f"{b}.kicad_pcb"
        fixture = pathlib.Path(a.fixtures) / b / "unrouted.kicad_pcb"
        if not routed.exists():
            return {"board": b, "error": "no routed board"}
        j = out / f"{b}.causes.json"
        cmd = [str(TM), "causes", str(routed), "--json", str(j), "--threads", str(a.threads), "--work", str(a.work), "--baseline-work",
               str(a.baseline_work), "--radius", str(a.radius)]
        if fixture.exists():
            cmd += ["--rules-from", str(fixture)]
        p = subprocess.run(cmd, capture_output=True, text=True)
        if p.returncode != 0 or not j.exists():
            return {"board": b, "error": (p.stderr or p.stdout).strip().splitlines()[-1:] or ["failed"]}
        c = json.loads(j.read_text())
        row = {"board": b, "layers": c["copper_layers"], "open_run": r.get("connections", 0) - r.get("routed", 0), "baseline": c["baseline"],
               "by_cause": c.get("by_cause", {}), "by_cause_dense": c.get("by_cause_at_dense_packages", {}), "dense_packages": c.get("dense_packages", 0)}
        if a.extra_layers > 0:
            more = scratch / b / "more_layers.kicad_pcb"
            more.parent.mkdir(parents=True, exist_ok=True)
            n = c["copper_layers"] + a.extra_layers
            lp = subprocess.run([str(TM), "layers", str(routed), "--count", str(n), "-o", str(more)], capture_output=True, text=True)
            if lp.returncode == 0:
                jm = scratch / b / "more_layers.causes.json"
                cmd = [str(TM), "causes", str(more), "--json", str(jm), "--baseline-only", "--baseline-work", str(a.baseline_work)]
                if fixture.exists():
                    cmd += ["--rules-from", str(fixture)]
                subprocess.run(cmd, capture_output=True, text=True)
                if jm.exists():
                    row["more_layers"] = {"layers": n, "open": json.loads(jm.read_text())["baseline"]["open"]}
            else:
                row["more_layers"] = {"layers": n, "refused": lp.stderr.strip()}
        print(f"{b}: {row['baseline']['open']} open, {row['by_cause']}", flush=True)
        return row

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        table = list(ex.map(one, rows))
    table.sort(key=lambda t: t["board"])
    (out / "causes.json").write_text(json.dumps(table, indent=1))

    tot: dict[str, int] = {}
    dense: dict[str, int] = {}
    lines = [f"# Open connections by cause ({a.name})", "",
             f"Run `{a.run}`; each open connection routed again alone at {a.work:,} work with other nets' copper removed near its ends "
             f"(within {a.radius} mm of an end pad) or away from them; the board as it is and a copy with {a.extra_layers} more inner layers "
             f"routed at {a.baseline_work:,} work.", "",
             "| Board | Layers | Open | Routes as is | " + " | ".join(CAUSES) + f" | Open with +{a.extra_layers} layers |",
             "|---|--:|--:|--:|" + "--:|" * len(CAUSES) + "--:|"]
    for t in table:
        if "error" in t:
            lines.append(f"| {t['board']} | error: {t['error']} |")
            continue
        for k, v in t["by_cause"].items():
            tot[k] = tot.get(k, 0) + v
        for k, v in t["by_cause_dense"].items():
            dense[k] = dense.get(k, 0) + v
        ml = t.get("more_layers", {})
        mls = str(ml["open"]) if "open" in ml else ("refused" if "refused" in ml else "-")
        lines.append(f"| {t['board']} | {t['layers']} | {t['open_run']} | {t['open_run'] - t['baseline']['open']} | "
                     + " | ".join(str(t["by_cause"].get(c, 0)) for c in CAUSES) + f" | {mls} |")
    ok = [t for t in table if "error" not in t]
    n_open = sum(t["baseline"]["open"] for t in ok)
    lines += ["", f"**Total** {sum(t['open_run'] for t in ok)} open in the run; {n_open} still open on the board as it is.", "",
              "| Cause | Open | At a dense package |", "|---|--:|--:|"]
    for c in CAUSES:
        lines.append(f"| {c} | {tot.get(c, 0)} | {dense.get(c, 0)} |")
    more = [t for t in ok if "open" in t.get("more_layers", {})]
    if more:
        lines += ["", f"With {a.extra_layers} more inner layers: {sum(t['baseline']['open'] for t in more)} → {sum(t['more_layers']['open'] for t in more)} "
                  f"open on {len(more)} boards ({sum(1 for t in more if t['more_layers']['open'] == 0)} complete)."]
    (out / "causes.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
