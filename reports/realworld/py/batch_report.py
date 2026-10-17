#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""batch_report.py RUN [RUN ...]: markdown table of reports/realworld/batch/RUN.jsonl (py/batch.py, finish_batch.py). With several runs the rows are merged by board (a later run replaces
an earlier one) and written to batch/summary.md; with one run to batch/RUN.md."""
import json
import pathlib
import sys

RW = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    runs = sys.argv[1:]
    merged: dict[str, dict] = {}
    for run in runs:
        f = RW / "batch" / f"{run}.jsonl"
        if f.exists():
            for l in f.read_text().splitlines():
                r = json.loads(l)
                r["_run"] = run
                merged[r["board"]] = r
    rows = list(merged.values())
    for r in rows:  # shorts: a track or via touching copper of another net (recomputed from the ledger); footprint-only pairs are the footprint's own
        lj = RW / "boards" / r["board"] / r.get("best_run", r["_run"]) / "ledger.json"
        if lj.exists():
            r["shorts"] = sum(1 for x in json.loads(lj.read_text())["added"] if x["type"] == "shorting_items" and any(i.startswith(("Track", "Via", "Arc")) for i in x.get("items", [])))
    ok = lambda r: r.get("kicad_open") == 0 and r.get("safety") and r["safety"] != "FAIL" and not r.get("shorts")
    routed = [r for r in rows if ok(r)]
    zero = [r for r in rows if r.get("open_best") == 0]
    lines = [f"# Batch {' + '.join(runs)}", "", f"{len(rows)} boards: **{len(routed)} routed and simulated** (KiCad 0 open, safety not FAIL, no shorts), {len(zero)} with 0 open in the loop's own judging, "
             f"{sum(1 for r in rows if r.get('error'))} errors, {sum(1 for r in rows if r.get('timeout'))} stopped by the time limit.", "",
             "| board | layers | open first round | open best | KiCad open | compromises | nets relaxed (power) | hard pours | parts moved | safety | s |", "|---|--:|--:|--:|--:|--:|--:|--:|--:|---|--:|"]
    for r in sorted(rows, key=lambda r: (r.get("open_best") if r.get("open_best") is not None else 9999, r["board"])):
        lines.append(f"| {r['board'][:44]} | {r.get('layers', '')} | {r.get('open_first', '')} | {r.get('open_best', '')} | {r.get('kicad_open', '')} | {r.get('compromises', '')} | "
                     f"{r.get('relaxed', '')} ({r.get('relaxed_power', '')}) | {r.get('hard', '')} | {r.get('moves', '')} | {r.get('safety', r.get('error', ''))}{' + ' + str(r['shorts']) + ' SHORTS' if r.get('shorts') else ''} | {r.get('seconds', '')} |")
    (RW / "batch" / ("summary.md" if len(runs) > 1 else f"{runs[0]}.md")).write_text("\n".join(lines) + "\n")
    print("\n".join(lines[:5]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
