#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Component-rules default gate on placed tiers (docs/15-component-rules.md §9.1 L3).

  bench/crules_tiers.py OFF_RUN SOFT_RUN [OFF_RUN SOFT_RUN ...]

Each pair is two bench/run.py runs over the same boards with --place routable and --place-crules off / soft.
Reports the routing gate (clean pass, completion, routed, per-board clean changes) and the design-intent distances
of bench/place_intent.py measured on the placed boards (roles bound on the input board), as medians over the boards
that have that kind of part, plus per-board wins and losses.
"""
import concurrent.futures as cf
import json
import pathlib
import statistics
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "bench"))
import place_intent  # noqa: E402

RES = ROOT / "bench/results"
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
KEYS = ["decap_median_mm", "crystal_median_mm", "esd_median_mm", "regcap_median_mm", "loadcap_median_mm", "xtal_rule_median_mm"]


def rows(run: str) -> dict:
    return {r["board"]: r for r in map(json.loads, (RES / run / "boards.jsonl").read_text().splitlines())}


def intent(run: str, board: str) -> dict:
    placed = RES / run / "boards" / f"{board}.placed.kicad_pcb"
    if not placed.exists():
        return {}
    try:
        return place_intent.metrics(placed, FIX / board / "unrouted.kicad_pcb")
    except Exception:  # noqa: BLE001
        return {}


def med(v):
    v = [x for x in v if x is not None]
    return round(statistics.median(v), 2) if v else None


def main() -> int:
    runs = sys.argv[1:]
    if len(runs) < 2 or len(runs) % 2:
        print(__doc__)
        return 2
    all_off, all_soft = {}, {}
    for off, soft in zip(runs[::2], runs[1::2]):
        a, b = rows(off), rows(soft)
        common = sorted(set(a) & set(b))
        ca, cb = sum(a[k].get("clean", False) for k in common), sum(b[k].get("clean", False) for k in common)
        ra, rb = sum(a[k].get("routed", 0) for k in common), sum(b[k].get("routed", 0) for k in common)
        pa = sum(1 for k in common if a[k].get("place") == "ok")
        pb = sum(1 for k in common if b[k].get("place") == "ok")
        print(f"{off} vs {soft}: {len(common)} boards, clean {ca} -> {cb}, routed {ra} -> {rb}, placements kept {pa} -> {pb}")
        for k in common:
            if a[k].get("clean") != b[k].get("clean"):
                print(f"   clean {a[k].get('clean')!s:5} -> {b[k].get('clean')!s:5}  {k}  (routed {a[k].get('routed')} -> {b[k].get('routed')}, "
                      f"place-added {a[k].get('place_added')} -> {b[k].get('place_added')})")
        with cf.ThreadPoolExecutor(8) as ex:
            ia = dict(zip(common, ex.map(lambda k: intent(off, k), common)))
            ib = dict(zip(common, ex.map(lambda k: intent(soft, k), common)))
        all_off.update({(off, k): v for k, v in ia.items()})
        all_soft.update({(off, k): v for k, v in ib.items()})
    print("\ndesign-intent distances (mm), medians over boards that have the part; off -> soft; per-board better/worse (>0.25 mm)")
    keys = sorted(all_off)
    for m in KEYS:
        both = [k for k in keys if all_off[k].get(m) is not None and all_soft[k].get(m) is not None]
        if not both:
            continue
        better = sum(1 for k in both if all_soft[k][m] < all_off[k][m] - 0.25)
        worse = sum(1 for k in both if all_soft[k][m] > all_off[k][m] + 0.25)
        print(f"  {m:22s} n={len(both):3d}  {med([all_off[k][m] for k in both])} -> {med([all_soft[k][m] for k in both])}   better {better}, worse {worse}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
