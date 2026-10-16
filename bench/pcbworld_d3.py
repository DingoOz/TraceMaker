#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Summarise a bench/run.py run on PCBWorld-Bench's D3 test boards beside the paper's published numbers (M17, doc 18 A1).

  bench/pcbworld_d3.py RUN [--fr RUN2] [--out FILE]   # RUN = bench/results/RUN (boards.jsonl) of `run.py --set pcbworld-d3-test`;
                                                       # RUN2 = `bench/pcbworld_fr.py` (Freerouting 2.1.0 with the paper's settings, this judge)

D3 is derived from PCBench (arXiv 2607.05915 section 4.2), so the 119 test boards (99 D3-A, 10 D3-B, 10 D3-C) are PCBench fixtures
and the run is `bench/run.py --set pcbworld-d3-test --time 120 --jobs 3 --threads 8`. Per class it prints
  * TraceMaker: boards clean (every connection routed and no error added, KiCad-judged), mean routability, boards open, and
    the boards where the 2.5.0-RC12 nightly Freerouting run (which comes with PCBench's fixtures) is clean and TraceMaker is not;
  * the paper's Table 3 (Freerouting 2.1.0, KRT, OrthoRoute, GPT-5.4, PPO; best of five rollouts selected by potential), copied
    as published. Those numbers are NOT comparable with ours without a caveat: the paper judges absolute DRVs under KiCad 9
    and its own relaxed design-rule files, KiCad's default rules otherwise; this judge counts errors the route adds under KiCad 10.
"""
from __future__ import annotations

import argparse
import json
import pathlib
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
SET = ROOT / "bench" / "sets" / "pcbworld-d3-test.txt"

# Table 3 of arXiv 2607.05915 (CP = clean pass, Rout. = routability), as published.
PUBLISHED = {
    "A": {"Reference (the human layout)": (1.00, 1.00), "Freerouting 2.1.0": (0.80, 0.91), "KiCadRoutingTools": (0.74, 0.94), "OrthoRoute": (0.02, 0.53),
          "GPT-5.4 (agent)": (0.65, 0.91), "PPO (trained on synthetic boards)": (0.86, 0.95)},
    "B": {"Reference (the human layout)": (1.00, 1.00), "Freerouting 2.1.0": (0.78, 1.00), "KiCadRoutingTools": (0.20, 0.86), "OrthoRoute": (0.00, 0.44),
          "GPT-5.4 (agent)": (0.00, 0.62), "PPO (trained on synthetic boards)": (0.45, 0.85)},
}


def classes() -> dict[str, list[str]]:
    out: dict[str, list[str]] = {}
    cur = ""
    for line in SET.read_text().splitlines():
        if line.startswith("# D3-"):
            cur = line[5:6]
        elif line.strip() and not line.startswith("#"):
            out.setdefault(cur, []).append(line.strip())
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("run")
    ap.add_argument("--fr", help="bench/results/RUN2 of bench/pcbworld_fr.py")
    ap.add_argument("--out", type=pathlib.Path)
    args = ap.parse_args()
    rows = {}
    for line in (ROOT / "bench" / "results" / args.run / "boards.jsonl").read_text().splitlines():
        r = json.loads(line)
        rows[r["board"]] = r
    cls = classes()
    out: list[str] = []
    out.append(f"# PCBWorld-Bench D3 test boards, run `{args.run}`\n")
    out.append("| class | boards | clean | clean % | mean routability | boards open | median s | Freerouting RC12 clean | TraceMaker clean, FR not | FR clean, TraceMaker not |")
    out.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    lost: list[tuple[str, str]] = []
    for c in sorted(cls):
        have = [rows[b] for b in cls[c] if b in rows]
        if not have:
            continue
        clean = sum(1 for r in have if r["clean"])
        fr_clean = sum(1 for r in have if r.get("fr_rc12", {}).get("clean"))
        gain = sum(1 for r in have if r["clean"] and not r.get("fr_rc12", {}).get("clean"))
        loss = [r for r in have if not r["clean"] and r.get("fr_rc12", {}).get("clean")]
        lost += [(c, r["board"]) for r in loss]
        out.append(f"| D3-{c} | {len(have)} | {clean} | {100 * clean / len(have):.1f} | {statistics.mean(r['completion'] for r in have):.3f} | "
                   f"{sum(1 for r in have if r['routed'] < r['connections'])} | {statistics.median(r['seconds'] for r in have):.1f} | "
                   f"{fr_clean} ({100 * fr_clean / len(have):.1f} %) | {gain} | {len(loss)} |")
    out.append("")
    if args.fr:
        fr: dict[str, list[dict]] = {}
        for line in (ROOT / "bench" / "results" / args.fr / "boards.jsonl").read_text().splitlines():
            r = json.loads(line)
            fr.setdefault(r["board"], []).append(r)
        runs = max(len(v) for v in fr.values())
        out.append(f"Freerouting 2.1.0 with the paper's settings (`-mp 10`, 4 GiB), {runs} runs per board, judged by the same judge as TraceMaker above:\n")
        out.append(f"| class | boards | clean, mean of runs (@1) | clean, any of {runs} runs (best of {runs}) | mean routability | median s per run | TraceMaker clean, FR best-of-{runs} not | FR best-of-{runs} clean, TraceMaker not |")
        out.append("|---|---:|---:|---:|---:|---:|---:|---:|")
        for c in sorted(cls):
            bs = [b for b in cls[c] if b in fr and b in rows]
            if not bs:
                continue
            at1 = statistics.mean(statistics.mean(1.0 if r.get("clean") else 0.0 for r in fr[b]) for b in bs)
            best = sum(1 for b in bs if any(r.get("clean") for r in fr[b]))
            rout = statistics.mean(statistics.mean(r.get("completion") or 0.0 for r in fr[b]) for b in bs)
            med = statistics.median(r.get("seconds") or 0.0 for b in bs for r in fr[b])
            gain = sum(1 for b in bs if rows[b]["clean"] and not any(r.get("clean") for r in fr[b]))
            loss = [b for b in bs if not rows[b]["clean"] and any(r.get("clean") for r in fr[b])]
            lost += [(c + " (vs Freerouting 2.1.0)", b) for b in loss]
            out.append(f"| D3-{c} | {len(bs)} | {at1:.3f} | {best} ({100 * best / len(bs):.1f} %) | {rout:.3f} | {med:.1f} | {gain} | {len(loss)} |")
        out.append("")
    out.append("Published (arXiv 2607.05915, Table 3; best of five rollouts, Freerouting and PPO the mean of four seeds; KiCad 9 and the paper's own rule files):\n")
    out.append("| method | D3-A CP | D3-A Rout. | D3-B CP | D3-B Rout. |")
    out.append("|---|---:|---:|---:|---:|")
    for m in PUBLISHED["A"]:
        a, b = PUBLISHED["A"][m], PUBLISHED["B"][m]
        out.append(f"| {m} | {a[0]:.2f} | {a[1]:.2f} | {b[0]:.2f} | {b[1]:.2f} |")
    out.append("")
    if lost:
        out.append("Boards TraceMaker loses (the nightly Freerouting 2.5.0-RC12 flag from PCBench's data, or a clean run of Freerouting 2.1.0 here):\n")
        for c, b in lost:
            r = rows[b]
            out.append(f"- D3-{c} `{b}`: routed {r['routed']}/{r['connections']}, added errors {r.get('added_errors') or 'none'}, {r['seconds']:.0f} s")
    text = "\n".join(out) + "\n"
    if args.out:
        args.out.write_text(text)
    print(text)
    return 0


if __name__ == "__main__":
    sys.exit(main())
