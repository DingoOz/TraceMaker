#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Arms of an option experiment on one board set, side by side (M17 C1: --txn and --relax on the failing PCBench tier B-D boards).

  bench/c1_table.py BASE ARM [ARM ...] [--out FILE]      # run names under bench/results (bench/run.py outputs)

Per arm against BASE: connections routed in total, boards routed more / fewer connections, boards clean, errors added, CPU seconds; then one row per board.
"""
import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load(name):
    return {r["board"]: r for r in (json.loads(l) for l in (ROOT / "bench/results" / name / "boards.jsonl").read_text().splitlines())}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("base")
    ap.add_argument("arms", nargs="+")
    ap.add_argument("--out", type=pathlib.Path)
    a = ap.parse_args()
    base = load(a.base)
    arms = {n: load(n) for n in a.arms}
    out = [f"| arm | boards | connections | routed | open | more than {a.base} | fewer | clean boards | errors added | CPU s |", "|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|"]
    for n, rows in [(a.base, base)] + list(arms.items()):
        more = sum(1 for b in base if rows[b]["routed"] > base[b]["routed"])
        fewer = sum(1 for b in base if rows[b]["routed"] < base[b]["routed"])
        conn = sum(r["connections"] for r in rows.values())
        routed = sum(r["routed"] for r in rows.values())
        out.append(f"| {n} | {len(rows)} | {conn} | {routed} | {conn - routed} | {more} | {fewer} | {sum(1 for r in rows.values() if r['clean'])} | "
                   f"{sum(sum(r['added_errors'].values()) for r in rows.values())} | {sum(r['cpu_s'] for r in rows.values()):.0f} |")
    out.append("")
    out.append("| board | connections | " + " | ".join([a.base] + a.arms) + " |")
    out.append("|---|---:|" + "---:|" * (1 + len(a.arms)))
    for b in sorted(base, key=lambda x: base[x]["connections"]):
        out.append(f"| {b[:44]} | {base[b]['connections']} | {base[b]['routed']} | " + " | ".join(str(arms[n][b]["routed"]) for n in a.arms) + " |")
    text = "\n".join(out) + "\n"
    if a.out:
        a.out.write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
