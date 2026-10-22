#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Table of the placement-closure experiment: the same boards routed on the human placement and on placements the placer produced.

  bench/place_closure.py BASE RUN [RUN ...] [--out FILE]      # bench/results/<name>/boards.jsonl of bench/run.py runs

The first run is the control (human placement). Per board and run: routed/connections, clean pass (KiCad-judged: every connection, no error
added by the routing or the placement), vias, track length, parts moved, signal HPWL before and after, and the placer's seconds.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load(name: str) -> dict[str, dict]:
    rows = {}
    for line in (ROOT / "bench" / "results" / name / "boards.jsonl").read_text().splitlines():
        r = json.loads(line)
        rows[r["board"]] = r
    return rows


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--out", type=pathlib.Path)
    a = ap.parse_args()
    runs = {n: load(n) for n in a.runs}
    base = runs[a.runs[0]]
    out = ["| board | connections | " + " | ".join(a.runs) + " |", "|---|--:|" + "--:|" * len(a.runs)]
    for b in base:
        cells = []
        for n in a.runs:
            r = runs[n].get(b)
            if not r:
                cells.append("–")
                continue
            tag = "**clean**" if r.get("clean") else f"{r['connections'] - r['routed']} open"
            if r.get("added_errors"):
                tag += f", +{sum(r['added_errors'].values())} err"
            if r.get("place_added"):
                tag += f", +{sum(r['place_added'].values())} placement err"
            cells.append(tag)
        out.append(f"| {b[:46]} | {base[b]['connections']} | " + " | ".join(cells) + " |")
    out += ["", "| run | boards clean | open connections (sum) | vias (sum) | track length mm (sum) | parts moved (sum) | HPWL after / before | placer s (sum) | route s (sum) |", "|---|--:|--:|--:|--:|--:|--:|--:|--:|"]
    for n in a.runs:
        rs = list(runs[n].values())
        length = sum((r.get("style") or {}).get("length_mm", 0) for r in rs)
        hb = sum(r.get("hpwl_before") or 0 for r in rs)
        ha = sum(r.get("hpwl_after") or 0 for r in rs)
        out.append(f"| {n} | {sum(1 for r in rs if r.get('clean'))}/{len(rs)} | {sum(r['connections'] - r['routed'] for r in rs)} | {sum(r.get('vias') or 0 for r in rs)} | "
                   f"{length:.0f} | {sum(r.get('moved') or 0 for r in rs)} | {(ha / hb if hb else 0):.3f} | {sum(r.get('place_s') or 0 for r in rs):.0f} | {sum(r.get('seconds') or 0 for r in rs):.0f} |")
    text = "\n".join(out) + "\n"
    if a.out:
        a.out.write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
