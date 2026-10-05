#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare 'placement + routing' runs (bench/run.py --place) with routing-only runs on the same boards.

  bench/compare_placed.py placed-tierA:mask-tierA placed-tierB:final10-tierB ...
"""
import importlib.util
import json
import pathlib
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("q", ROOT / "bench/quality.py")
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)


def rows(run):
    return {json.loads(l)["board"]: json.loads(l) for l in open(ROOT / "bench/results" / run / "boards.jsonl")}


def main():
    print("| Tier | Boards | Clean: route only | Clean: place + route | Completion: route only | Completion: place + route |"
          " Placement fell back | Placement added errors | Median placement time | Length ratio | Via ratio |")
    print("|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|")
    for pair in sys.argv[1:]:
        placed, base = pair.split(":")
        P, B = rows(placed), rows(base)
        common = sorted(set(P) & set(B))
        if not common:
            continue
        cp = sum(1 for b in common if P[b].get("clean"))
        cb = sum(1 for b in common if B[b].get("clean"))
        mp = statistics.mean(P[b].get("completion") or 0 for b in common)
        mb = statistics.mean(B[b].get("completion") or 0 for b in common)
        fell = sum(1 for b in common if not str(P[b].get("place", "")).startswith("ok"))
        padd = sum(1 for b in common if P[b].get("place_added"))
        pt = statistics.median(P[b].get("place_s") or 0 for b in common)
        lr, vr = [], []
        for b in common:
            if P[b].get("completion") == 1.0 and B[b].get("completion") == 1.0:
                gp = q.geometry(q.board_json(ROOT / "bench/results" / placed / "boards" / f"{b}.kicad_pcb"))
                gb = q.geometry(q.board_json(ROOT / "bench/results" / base / "boards" / f"{b}.kicad_pcb"))
                if gb["length_mm"]:
                    lr.append(gp["length_mm"] / gb["length_mm"])
                if gb["vias"]:
                    vr.append(gp["vias"] / gb["vias"])
        tier = placed.split("tier")[-1]
        print(f"| {tier} | {len(common)} | {100*cb/len(common):.1f}% | {100*cp/len(common):.1f}% | {100*mb:.1f}% | {100*mp:.1f}% | {fell} | {padd} | {pt:.0f} s |"
              f" {statistics.median(lr):.2f} | {statistics.median(vr) if vr else float('nan'):.2f} |")


if __name__ == "__main__":
    main()
