#!/usr/bin/env python3
"""Detection quality of component-aware layout rules (docs/15-component-rules.md §9.1 L1).

  bench/crules_detect.py [--labels bench/crules_labels.json] [--json out.json] [--jobs 4] [-v]

Runs `tracemaker rules <board> --json` on every labelled PCBench board and compares the detected instances with
the hand labels in bench/crules_labels.json, per category: precision and recall at the suggest threshold (what the
report lists and what soft rules use) and at the apply threshold (what hard rules and keep-outs use). References
listed under "ignore" count neither way. Precision first: a wrong detection applies wrong rules.
"""
import argparse
import collections
import concurrent.futures
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = ROOT / "build/release/src/app/tracemaker"
OUT = ROOT / "build/crules"


def run_board(board: str, fixtures: str) -> dict:
    path = ROOT / fixtures.replace("<board>", board)
    out = OUT / (board.replace(" ", "_") + ".rules.json")
    OUT.mkdir(parents=True, exist_ok=True)
    r = subprocess.run([str(TM), "rules", str(path), "--json", str(out)], capture_output=True, text=True)
    if r.returncode != 0:
        raise RuntimeError(f"{board}: tracemaker rules failed: {r.stderr.strip()}")
    return json.loads(out.read_text())


def score(labels: dict, results: dict, categories: list, threshold_key: str, thresholds: dict, verbose: bool) -> dict:
    tp, fp, fn = collections.Counter(), collections.Counter(), collections.Counter()
    errors = collections.defaultdict(list)
    for board, spec in labels["boards"].items():
        rep = results[board]
        lab = spec.get("labels", {})
        ign = spec.get("ignore", {})
        cats = list(categories) + (["ic_decoupling"] if "ic_decoupling" in lab else [])
        for cat in cats:
            truth = collections.Counter(lab.get(cat, []))
            pred = collections.Counter(i["anchor"] for i in rep["instances"] if i["category"] == cat and i["confidence"] >= thresholds[threshold_key])
            for ref in ign.get(cat, []):
                truth.pop(ref, None)
                pred.pop(ref, None)
            hit = truth & pred
            tp[cat] += sum(hit.values())
            fp[cat] += sum((pred - truth).values())
            fn[cat] += sum((truth - pred).values())
            for ref in (pred - truth):
                errors[cat].append(f"FP {board}:{ref}")
            for ref in (truth - pred):
                errors[cat].append(f"FN {board}:{ref}")
    rows = {}
    for cat in list(categories) + ["ic_decoupling"]:
        p = tp[cat] / (tp[cat] + fp[cat]) if tp[cat] + fp[cat] else None
        r = tp[cat] / (tp[cat] + fn[cat]) if tp[cat] + fn[cat] else None
        rows[cat] = {"tp": tp[cat], "fp": fp[cat], "fn": fn[cat], "precision": p, "recall": r, "errors": errors[cat] if verbose else errors[cat][:12]}
    return rows


def fmt(x):
    return "  -  " if x is None else f"{x:.3f}"


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--labels", default=str(ROOT / "bench/crules_labels.json"))
    ap.add_argument("--json")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("-v", "--verbose", action="store_true", help="print every false positive/negative")
    a = ap.parse_args()
    labels = json.loads(pathlib.Path(a.labels).read_text())
    if not TM.exists():
        print(f"error: {TM} not built", file=sys.stderr)
        return 1
    boards = sorted(labels["boards"])
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        results = dict(zip(boards, ex.map(lambda b: run_board(b, labels["fixtures"]), boards)))
    thresholds = next(iter(results.values()))["thresholds"]
    report = {"boards": len(boards), "thresholds": thresholds}
    for key in ("suggest", "apply"):
        rows = score(labels, results, labels["categories"], key, thresholds, a.verbose)
        report[key] = rows
        print(f"\nconfidence >= {thresholds[key]} ({key}), {len(boards)} boards")
        print(f"{'category':18s} {'TP':>4s} {'FP':>4s} {'FN':>4s}  precision  recall")
        for cat, r in rows.items():
            if r["tp"] + r["fp"] + r["fn"] == 0:
                continue
            print(f"{cat:18s} {r['tp']:4d} {r['fp']:4d} {r['fn']:4d}  {fmt(r['precision']):>9s}  {fmt(r['recall']):>6s}")
        if key == "suggest":
            for cat, r in rows.items():
                for e in r["errors"]:
                    print(f"   {cat:16s} {e}")
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps(report, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
