#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Zone refill parity (doc 05 §36): `tracemaker drc --refill-zones` against `kicad-cli pcb drc --refill-zones`.

  bench/refill_parity.py BOARD.kicad_pcb ... [--dir DIR [--glob G]] [--sample N] [--jobs J] [--out FILE.json]

A board passes when both judges report the same number of unconnected items in every net (the representative
items of a missing connection are chosen differently, the clusters are the same). Boards without copper zones
are skipped. --dir takes every *.kicad_pcb below DIR (PCBench: bench/data/freerouting/scripts/benchmark/fixtures/
PCBench); --sample N keeps N of them, evenly spaced in name order. Each board is judged in its own temporary copy
with its project files, one kicad-cli per board. Exit status 1 when a board differs.
"""
import argparse
import collections
import concurrent.futures as cf
import json
import os
import pathlib
import re
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = os.environ.get("TM_BINARY", str(ROOT / "build/release/src/app/tracemaker"))
NET = re.compile(r"\[([^\]]*)\]")


def per_net(report: dict) -> collections.Counter:
    c = collections.Counter()
    for u in report.get("unconnected_items", []):
        m = NET.search(u["items"][0]["description"]) if u.get("items") else None
        # KiCad escapes '/' as {slash} in descriptions and writes KiCad 5 overbars ~X as ~{X}
        c[(m.group(1) if m else "?").replace("{slash}", "/").replace("{", "").replace("}", "")] += 1
    return c


def judge(board: pathlib.Path) -> dict:
    with tempfile.TemporaryDirectory(prefix="tmk-refill-") as td:
        tmp = pathlib.Path(td)
        for f in board.parent.iterdir():  # the board with its .kicad_pro/.kicad_dru, under the same name
            if f.name.startswith(board.stem + ".") and f.suffix in (".kicad_pcb", ".kicad_pro", ".kicad_dru"):
                shutil.copy(f, tmp / f.name)
        pcb = tmp / board.name
        t0 = time.time()
        subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "--refill-zones", "-o", str(tmp / "k.json"), str(pcb)],
                       capture_output=True, timeout=1800)
        t1 = time.time()
        subprocess.run([TM, "drc", str(pcb), "--refill-zones", "--json", str(tmp / "t.json")], capture_output=True, timeout=1800)
        t2 = time.time()
        if not (tmp / "k.json").exists() or not (tmp / "t.json").exists():
            return {"board": str(board), "error": "a judge produced no report"}
        k, t = per_net(json.loads((tmp / "k.json").read_text())), per_net(json.loads((tmp / "t.json").read_text()))
        diff = {n: [t[n], k[n]] for n in sorted(set(k) | set(t)) if k[n] != t[n]}
        return {"board": str(board), "kicad": sum(k.values()), "tracemaker": sum(t.values()), "nets_differing": diff,
                "kicad_s": round(t1 - t0, 2), "tracemaker_s": round(t2 - t1, 2)}


def name(board: str) -> str:
    p = pathlib.Path(board)
    return f"{p.parent.name}/{p.name}"


def has_zones(p: pathlib.Path) -> bool:
    text = p.read_text(errors="replace")
    return len(re.findall(r"\(zone\s", text)) > text.count("(keepout")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("boards", nargs="*", type=pathlib.Path)
    ap.add_argument("--dir", type=pathlib.Path)
    ap.add_argument("--glob", default="*.kicad_pcb", help="file names taken below --dir (PCBench: raw.kicad_pcb)")
    ap.add_argument("--sample", type=int, default=0)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--out", type=pathlib.Path)
    a = ap.parse_args()
    boards = list(a.boards)
    if a.dir:
        boards += sorted(a.dir.rglob(a.glob))
    boards = [b for b in boards if has_zones(b)]
    if a.sample and len(boards) > a.sample:
        step = len(boards) / a.sample
        boards = [boards[int(i * step)] for i in range(a.sample)]
    if not boards:
        print("no boards with zones")
        return 0
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for r in ex.map(judge, boards):
            rows.append(r)
            ok = "error" not in r and not r["nets_differing"]
            detail = r.get("error") or (f"unconnected tracemaker {r['tracemaker']} kicad {r['kicad']}, "
                                        f"{r['tracemaker_s']} s vs {r['kicad_s']} s" + ("" if ok else f", differ: {r['nets_differing']}"))
            print(f"{'same' if ok else 'DIFF'} {name(r['board'])[:60]:60} {detail}")
    same = sum(1 for r in rows if "error" not in r and not r["nets_differing"])
    print(f"{same}/{len(rows)} boards with the same unconnected items per net")
    if a.out:
        a.out.write_text(json.dumps(rows, indent=1))
    return 0 if same == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
