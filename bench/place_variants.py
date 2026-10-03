#!/usr/bin/env python3
"""Compare annealer variants of `tracemaker-place` (placement only, no routing) on PCBench boards.

The figure of merit is the annealing cost the placer minimises, in signal-mm: weighted HPWL (signal nets x1,
power nets x0.1) + alpha * airwire crossings (alpha = 2 mm). Every variant gets the same move budget.

  bench/place_variants.py --mode refine --jobs 4 --threads 4 --variant sa: --variant pt:--tempering Board1 Board2 ...
"""
import argparse
import concurrent.futures as cf
import json
import pathlib
import statistics
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
PLACE = ROOT / "build/release/src/place/tracemaker-place"


def one(board: str, mode: str, label: str, flags: list, out: pathlib.Path, threads: int, seed: int) -> dict:
    js = out / f"{board}.{mode}.{label}.s{seed}.json"
    pcb = out / f"{board}.{mode}.{label}.s{seed}.kicad_pcb"
    subprocess.run([str(PLACE), str(FIX / board / "unrouted.kicad_pcb"), "-o", str(pcb), "--mode", mode, "--threads", str(threads),
                    "--seed", str(seed), "--json", str(js)] + flags, capture_output=True, text=True)
    try:
        j = json.loads(js.read_text())
        a = j["after"]
        return {"board": board, "label": label, "seed": seed, "cost": a["weighted_hpwl_mm"] + 2 * a["crossings"], "hpwl": a["hpwl_mm"],
                "seconds": j["seconds_total"], "legal": j["legal"]}
    except Exception as e:  # noqa: BLE001
        return {"board": board, "label": label, "seed": seed, "error": str(e)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--mode", default="refine")
    ap.add_argument("--variant", action="append", required=True, help="label:flags (flags separated by spaces)")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--seeds", type=int, default=1)
    ap.add_argument("--out", default=str(ROOT / "build/place-variants"))
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    out.mkdir(parents=True, exist_ok=True)
    variants = [(v.split(":", 1)[0], v.split(":", 1)[1].split()) for v in a.variant]
    tasks = [(b, l, f, s) for b in a.boards for (l, f) in variants for s in range(1, a.seeds + 1)]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(lambda t: one(t[0], a.mode, t[1], t[2], out, a.threads, t[3]), tasks))
    by = {}
    for r in rows:
        by.setdefault((r["board"], r["label"]), []).append(r)
    labels = [l for l, _ in variants]
    print("| Board | " + " | ".join(f"{l} cost" for l in labels) + " | " + " | ".join(f"{l} s" for l in labels) + " |")
    print("|---|" + "--:|" * (2 * len(labels)))
    ratios = {l: [] for l in labels}
    for b in a.boards:
        cells, secs = [], []
        base = None
        for l in labels:
            rs = [r for r in by.get((b, l), []) if "error" not in r]
            if not rs:
                cells.append("err")
                secs.append("")
                continue
            c = statistics.mean(r["cost"] for r in rs)
            base = c if base is None else base
            ratios[l].append(c / base if base else 1.0)
            cells.append(f"{c:.1f}" + ("" if all(r["legal"] for r in rs) else " (illegal)"))
            secs.append(f"{statistics.mean(r['seconds'] for r in rs):.1f}")
        print(f"| {b} | " + " | ".join(cells) + " | " + " | ".join(secs) + " |")
    print()
    for l in labels:
        if ratios[l]:
            g = statistics.geometric_mean([max(x, 1e-9) for x in ratios[l]])
            print(f"{l}: geometric-mean cost ratio vs {labels[0]} = {g:.4f}; better on {sum(x < 0.9999 for x in ratios[l])}, "
                  f"worse on {sum(x > 1.0001 for x in ratios[l])} of {len(ratios[l])}")
    (out / f"summary.{a.mode}.json").write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
