#!/usr/bin/env python3
"""Estimate what a late reachability proof saves, from per-search logs (profile/*-searches.txt.gz).

Each line: PROBE found expanded soft open_exhausted touched_edge capped distinct_cells window_cells, written by a
throwaway build of 455e315 that printed one line per search (one variant, 20 M work). A failed strict search that
exhausted its region costs T + (its region) instead of its expansions; a long success or a capped failure pays a flood
(its distinct cells as an optimistic estimate, the window as a pessimistic one)."""
import gzip, pathlib, sys

here = pathlib.Path(__file__).resolve().parent.parent / "profile"
for f in sorted(here.glob("*-searches.txt.gz")):
    rows = [list(map(int, l.split()[1:])) for l in gzip.open(f, "rt")]
    tot = sum(r[1] for r in rows)
    strict = [r for r in rows if r[2] == 0]
    exh = [r for r in strict if not r[0] and r[3] and not r[5]]
    cap = [r for r in strict if r[5]]
    for T in (20_000, 50_000, 100_000):
        save = sum(max(0, r[1] - (T + r[6])) for r in exh if r[1] > T)
        opt = sum(r[6] for r in strict if r[0] and r[1] > T) + sum(r[6] for r in cap)
        pes = sum(r[7] for r in strict if r[0] and r[1] > T) + sum(r[7] for r in cap)
        print(f"{f.name.split('-searches')[0][:30]:30s} T={T:>7}: saves {save / tot:5.1%}, flood cost {opt / tot:5.1%} .. {pes / tot:5.1%} of the budget")
