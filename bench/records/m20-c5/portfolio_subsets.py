#!/usr/bin/env python3
"""Which portfolio variants should search from the crowded end (doc 05 section 55, D143)?

Every variant of the portfolio runs with the whole work budget and keeps its own result, so a portfolio in which some variants
search from the crowded end and the others do not can be scored from two runs, one with the option nowhere and one with it in every
variant: per board, the routed count of the mixed portfolio is the best of the chosen variants' counts. variants.json holds each
board's per-variant routed counts (from bench/results/<run>/boards/*.route.json).
usage: portfolio_subsets.py BASE:CROWDED [BASE:CROWDED ...]   e.g. c5-base:c5-crowdpads c5c-base:c5c-crowded"""
import json
import pathlib
import sys

runs = json.loads((pathlib.Path(__file__).parent / "variants.json").read_text())
pairs = [p.split(":") for p in sys.argv[1:]]
rows = []
for mask in range(256):
    opn = lost = gained = better = worse = 0
    for base, crowded in pairs:
        a, b = runs[base], runs[crowded]
        for board in sorted(set(a) & set(b)):
            n = a[board]["connections"]
            va, vb = a[board]["routed_by_variant"], b[board]["routed_by_variant"]
            best = max(va)
            mixed = max((vb if mask >> i & 1 else va)[i] for i in range(len(va)))
            opn += n - mixed
            lost += best == n and mixed < n
            gained += best < n and mixed == n
            better += mixed > best
            worse += mixed < best
    rows.append((mask, opn, lost, gained, better, worse))
print("| variants with the crowded source | open | complete boards lost | gained | boards better | worse |")
print("|---|--:|--:|--:|--:|--:|")
show = [rows[0], rows[255]] + sorted(rows[1:255], key=lambda r: (r[2] > 0, r[1]))[:10]
for mask, *r in show:
    on = ", ".join(str(i) for i in range(8) if mask >> i & 1) or "none"
    print(f"| {on} | " + " | ".join(str(x) for x in r) + " |")
