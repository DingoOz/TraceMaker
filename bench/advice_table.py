#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Tabulate bench/advice_eval.py runs side by side: connections routed per board, one column per run.

  bench/advice_table.py plain=adv-a1:plain control=adv-c1 mixed=adv-a1 stuck-first=adv-a2

Each argument is LABEL=RUN or LABEL=RUN:plain (the plain run's count recorded in RUN, before any advice). The control run has no advisor
and its counts are its final counts. Prints a markdown table with totals.
"""
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def main():
    cols = []
    for arg in sys.argv[1:]:
        label, spec = arg.split("=", 1)
        run, _, which = spec.partition(":")
        rows = {r["board"]: r for r in json.loads((ROOT / "bench/results" / run / "advice.json").read_text())["rows"]}
        cols.append((label, {b: (r["plain"] if which == "plain" else r["routed"]) for b, r in rows.items()}, rows))
    boards = sorted(cols[0][1])
    conns = {b: cols[0][2][b]["connections"] for b in boards}
    print("| board | connections | " + " | ".join(c[0] for c in cols) + " |")
    print("|---|---:|" + "---:|" * len(cols))
    totals = [0] * len(cols)
    for b in boards:
        vals = [c[1][b] for c in cols]
        totals = [t + v for t, v in zip(totals, vals)]
        print(f"| {b} | {conns[b]} | " + " | ".join(str(v) for v in vals) + " |")
    print(f"| **total** | {sum(conns.values())} | " + " | ".join(f"**{t}**" for t in totals) + " |")


if __name__ == "__main__":
    sys.exit(main())
