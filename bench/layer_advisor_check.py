#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The layer-count advisor against a brute-force run (doc 05 §53, M20 C3 gate).

The advisor (`tracemaker route --try-layers 2,4,6`) routes each count, fewest first, and stops at the first the engine
judges complete. The brute force makes a copy of every board with each count (`tracemaker layers --count N`), routes
them all with `bench/run.py` and lets KiCad judge each (zones refilled); its answer is the fewest count KiCad calls
clean. The two answers must match on every board.

  bench/layer_advisor_check.py --set pcbench-bd-fail2 --counts 2,4,6 --work 10000000 --name c3-layers [--route-args "..."]

Writes bench/results/<name>/{layer_check.json, layer_check.md}; the brute-force runs are bench/results/<name>-L<N>.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import os
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import viewport  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", required=True, help="board set in bench/sets/<SET>.txt (PCBench fixtures)")
    ap.add_argument("--counts", default="2,4,6")
    ap.add_argument("--work", type=int, required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--route-args", default="", help="extra route options for both the advisor and the brute force")
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--view", action="store_true", help="every route streams to a live viewer (bench/viewport.py)")
    ap.add_argument("--skip-brute", action="store_true", help="reuse the brute-force runs already in bench/results")
    a = ap.parse_args()
    counts = sorted({int(c) for c in a.counts.split(",")})
    boards = [x.strip() for x in (ROOT / "bench/sets" / f"{a.set}.txt").read_text().splitlines() if x.strip() and not x.startswith("#")]
    out = ROOT / "bench/results" / a.name
    out.mkdir(parents=True, exist_ok=True)
    extra = a.route_args.split()

    # The brute force: a fixture directory per count, each board converted, then a judged run per count.
    takes: dict[tuple[str, int], str] = {}  # (board, count) -> "" when the board takes the count, else why not
    for n in counts:
        fixtures = ROOT / "build/scratch/layer-check" / a.name / f"L{n}"
        for b in boards:
            dst = fixtures / b / "unrouted.kicad_pcb"
            dst.parent.mkdir(parents=True, exist_ok=True)
            r = subprocess.run([str(TM), "layers", str(FIX / b / "unrouted.kicad_pcb"), "--count", str(n), "-o", str(dst)], capture_output=True, text=True)
            takes[(b, n)] = "" if r.returncode == 0 else (r.stderr.strip() or "refused")
        ok = [b for b in boards if not takes[(b, n)]]
        if a.skip_brute or not ok:
            continue
        cmd = [sys.executable, str(ROOT / "bench/run.py"), "--fixtures", str(fixtures), "--boards", *ok, "--work", str(a.work), "--refill",
               "--jobs", str(a.jobs), "--threads", str(a.threads), "--name", f"{a.name}-L{n}"] + (["--view"] if a.view else [])
        if extra:
            cmd.append("--route-args=" + " ".join(extra))
        subprocess.run(cmd, check=True)
    kicad: dict[tuple[str, int], dict] = {}
    for n in counts:
        rows = ROOT / "bench/results" / f"{a.name}-L{n}" / "boards.jsonl"
        if rows.exists():
            for line in rows.read_text().splitlines():
                r = json.loads(line)
                kicad[(r["board"], n)] = r

    # The advisor, on the original boards.
    def advise(b: str) -> dict:
        d = out / "advisor" / b
        d.mkdir(parents=True, exist_ok=True)
        cmd = [str(TM), "route", str(FIX / b / "unrouted.kicad_pcb"), "-o", str(d / "out.kicad_pcb"), "--try-layers", ",".join(map(str, counts)),
               "--work", str(a.work), "--no-kb", "--threads", str(a.threads), "--time", "3600", "--json", str(d / "route.json")] + extra
        view = viewport.route_args(True) if a.view else []
        r = subprocess.run(cmd + view, capture_output=True, text=True)
        if view:
            viewport.release_port(viewport.port_of(view))
        (d / "log.txt").write_text(r.stdout + r.stderr)
        s = json.loads((d / "route.json").read_text()) if (d / "route.json").exists() else {}
        return {"board": b, "exit": r.returncode, "layers": s.get("layers")}

    with cf.ThreadPoolExecutor(a.jobs) as ex:
        advised = {r["board"]: r for r in ex.map(advise, boards)}

    table = []
    for b in boards:
        clean = {n: kicad.get((b, n), {}).get("clean") for n in counts}
        brute = next((n for n in counts if clean[n]), None)
        lay = advised[b].get("layers") or {}
        adv = lay.get("chosen") if lay.get("complete") else None
        table.append({"board": b, "kicad_clean": clean, "refused": {n: takes[(b, n)] for n in counts if takes[(b, n)]},
                      "brute_force": brute, "advisor": adv, "tried": lay.get("tried"), "match": brute == adv})
    (out / "layer_check.json").write_text(json.dumps(table, indent=1))
    head = "| Board | " + " | ".join(f"{n} layers (KiCad)" for n in counts) + " | Brute force | Advisor | Match |"
    lines = [f"# Layer advisor check {a.name}", "", f"Set {a.set}, counts {a.counts}, {a.work:,} work units"
             + (f", route options `{a.route_args}`" if a.route_args else "") + "; KiCad refills zones before judging.", "", head,
             "|---|" + "---|" * len(counts) + "--:|--:|---|"]
    for t in table:
        cells = ["not tried" if t["refused"].get(n) else ("clean" if t["kicad_clean"][n] else ("open" if t["kicad_clean"][n] is not None else "?")) for n in counts]
        lines.append(f"| {t['board']} | " + " | ".join(cells) + f" | {t['brute_force'] or 'none'} | {t['advisor'] or 'none'} | {'yes' if t['match'] else 'NO'} |")
    matched = sum(t["match"] for t in table)
    lines += ["", f"Answers match on {matched} of {len(table)} boards."]
    (out / "layer_check.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if matched == len(table) else 1


if __name__ == "__main__":
    sys.exit(main())
