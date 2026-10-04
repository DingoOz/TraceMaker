#!/usr/bin/env python3
"""Clean pass of finished runs split by escape feasibility (doc 05 §12): boards whose dense-package pins can all
escape under the board's own rules vs boards with pins no router can get out (`tracemaker escape`).

  bench/feasibility.py RUN [RUN ...] [--analysis build/escape_all]

The per-board analysis JSONs come from `tracemaker escape <board> --json build/escape_all/<board>.json`
(see build/escape_all/run.sh); boards without one are listed as unknown.
"""
import argparse
import json
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--analysis", default=str(ROOT / "build/escape_all"))
    a = ap.parse_args()
    an = pathlib.Path(a.analysis)
    print("| Run | Boards | Clean | Feasible boards | Clean (feasible) | Infeasible boards |")
    print("|---|--:|--:|--:|--:|---|")
    for rid in a.runs:
        rows = [json.loads(l) for l in (ROOT / "bench/results" / rid / "boards.jsonl").read_text().splitlines() if l.strip()]
        rows = [r for r in rows if "clean" in r]
        dead = {}
        for r in rows:
            if r.get("dead_pins") is not None:
                dead[r["board"]] = r["dead_pins"]
            elif (an / f"{r['board']}.json").exists():
                dead[r["board"]] = json.loads((an / f"{r['board']}.json").read_text())["dead"]
        feas = [r for r in rows if dead.get(r["board"]) == 0]
        bad = [f"{r['board']} ({dead[r['board']]})" for r in rows if dead.get(r["board"], 0) > 0]
        cf = sum(r["clean"] for r in feas) / len(feas) if feas else 0
        print(f"| {rid} | {len(rows)} | {sum(r['clean'] for r in rows) / len(rows):.1%} | {len(feas)} | {cf:.1%} | {', '.join(bad)} |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
