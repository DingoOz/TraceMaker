#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the frozen placement sets of doc 14 §3: H (held-out) and S (from scratch).

  bench/make_place_sets.py select [--jobs 6]     -> bench/place_sets/H.txt (never change it afterwards)
  bench/make_place_sets.py scratch               -> bench/data/place_sets/S/<board>/unrouted.kicad_pcb

H (§3.1): a seeded sample (seed 7) of PCBench boards that appear in no earlier run or development list, have at
least 10 movable parts and at most 300 parts, a courtyard on at least 80 % of their parts, and that TraceMaker routes
completely in the human placement in 120 s (8 variants). 20 boards at or below the median part count of the
candidates and 20 above.

S: the H boards with every part that full mode may move (tracemaker-place --mode full, component rules off) moved to the centre of
the board outline; rotation and side stay, everything else is untouched. The human placement is no longer in the file.
"""
import argparse
import concurrent.futures as cf
import json
import pathlib
import random
import re
import statistics
import subprocess
import tempfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = ROOT / "build/release/src/app/tracemaker"
PLACE = ROOT / "build/release/src/place/tracemaker-place"
SETS = ROOT / "bench/place_sets"
SDIR = ROOT / "bench/data/place_sets/S"


def used_boards() -> set[str]:
    """Every board name that appears in a result, a development list or a build directory of an earlier run."""
    names = {p.name for p in FIX.iterdir() if p.is_dir()}
    seen: set[str] = set()
    for f in list((ROOT / "bench/results").glob("*/*.jsonl")) + list((ROOT / "bench").glob("*.json")) + list((ROOT / "build").glob("*.txt")):
        text = f.read_text(errors="replace")
        seen |= {n for n in names if n in text}
    for d in (ROOT / "build").iterdir():  # build/place*/<board>/, build/m6*/<config>/<board>.kicad_pcb, ...
        # escape_all*: the escape-feasibility census of all 1,157 boards (doc 05 §12), a static analysis with no
        # tuning against it; counting it would leave no held-out boards at all.
        if d.is_dir() and d.name not in ("release", "asan", "tsan", "cpu-only", "debug", "tools") and not d.name.startswith("escape_all"):
            for p in d.rglob("*"):
                stem = p.name.split(".kicad_pcb")[0].split(".")[0] if p.is_file() else p.name
                if p.name in names:
                    seen.add(p.name)
                elif stem in names:
                    seen.add(stem)
    for f in list((ROOT / "bench").glob("*.py")) + list((ROOT / "tests").rglob("*")) + list((ROOT / "docs").glob("*.md")):
        if f.is_file():
            text = f.read_text(errors="replace")
            seen |= {n for n in names if n in text}
    return seen


def placer_report(board: pathlib.Path, mode: str = "refine") -> dict | None:
    """The placer's view of a board. mode "full" (no fallbacks, no component rules) gives the parts full mode itself
    would move: it also holds parts that overhang the board edge in the input, which refine mode may move."""
    with tempfile.TemporaryDirectory() as td:
        js = pathlib.Path(td) / "p.json"
        subprocess.run([str(PLACE), str(board), "-o", str(pathlib.Path(td) / "p.kicad_pcb"), "--mode", mode, "--effort", "0.001", "--threads", "1",
                        "--component-rules", "off", "--no-fallback", "--json", str(js)], capture_output=True, timeout=600)
        return json.loads(js.read_text()) if js.exists() else None


def eligible(name: str) -> dict | None:
    """Static criteria and the 120 s routing check; None if the board does not qualify."""
    meta = json.loads((FIX / name / "metadata.normalized.json").read_text())
    parts = meta.get("board", {}).get("components") or 0
    if not 10 <= parts <= 300:
        return None
    src = FIX / name / "unrouted.kicad_pcb"
    try:
        rep = placer_report(src)
    except subprocess.TimeoutExpired:
        return None
    if not rep or rep.get("movable", 0) < 10 or rep.get("parts", 0) > 300:
        return None
    missing = 0
    for note in rep.get("notes", []):
        m = re.match(r"(\d+) footprint\(s\) without courtyard", note)
        if m:
            missing = int(m.group(1))
    if missing > 0.2 * rep["parts"]:
        return None
    with tempfile.TemporaryDirectory() as td:
        js = pathlib.Path(td) / "r.json"
        subprocess.run([str(TM), "route", str(src), "-o", str(pathlib.Path(td) / "r.kicad_pcb"), "--time", "120", "--threads", "8", "--no-kb", "--json", str(js)],
                       capture_output=True, timeout=600)
        if not js.exists():
            return None
        r = json.loads(js.read_text())
    if r["connections"] == 0 or r["routed"] != r["connections"]:
        return None
    return {"board": name, "parts": rep["parts"], "movable": rep["movable"], "connections": r["connections"]}


def select(jobs: int) -> int:
    out = SETS / "H.txt"
    if out.exists():
        print(f"{out} exists: the held-out set is frozen; a new set needs a new name")
        return 1
    used = used_boards()
    cands = sorted(p.name for p in FIX.iterdir() if p.is_dir() and p.name not in used and (p / "unrouted.kicad_pcb").exists())
    sizes = {}
    for n in cands:
        c = json.loads((FIX / n / "metadata.normalized.json").read_text()).get("board", {}).get("components") or 0
        if 10 <= c <= 300:
            sizes[n] = c
    cands = sorted(sizes)
    median = statistics.median(sizes.values())
    random.Random(7).shuffle(cands)
    print(f"{len(used)} boards used before, {len(cands)} candidates with 10..300 parts, median {median} parts")
    low: list[dict] = []
    high: list[dict] = []
    tried = 0
    with cf.ThreadPoolExecutor(jobs) as ex:
        for i in range(0, len(cands), jobs):
            if len(low) >= 20 and len(high) >= 20:
                break
            chunk = [n for n in cands[i:i + jobs] if (len(low) < 20 and sizes[n] <= median) or (len(high) < 20 and sizes[n] > median)]
            tried += len(chunk)
            for r in ex.map(eligible, chunk):  # in sample order, so the list does not depend on timing
                if r is None:
                    continue
                side = low if sizes[r["board"]] <= median else high
                if len(side) < 20:
                    side.append(r)
            print(f"tried {tried}: {len(low)} small, {len(high)} large", flush=True)
    rows = low + high
    SETS.mkdir(exist_ok=True)
    out.write_text("# Held-out placement set H (doc 14 §3.1), seed 7. Frozen: never edit.\n" + "".join(r["board"] + "\n" for r in rows))
    (SETS / "H.json").write_text(json.dumps({"seed": 7, "median_parts": median, "tried": tried, "boards": rows}, indent=1))
    print(f"wrote {out}: {len(rows)} boards ({len(low)} small, {len(high)} large)")
    return 0 if len(rows) == 40 else 2


def footprint_spans(text: str) -> list[tuple[int, int]]:
    """(start, end) of every top-level (footprint ...) / (module ...) list."""
    spans = []
    for m in re.finditer(r"\n  \((?:footprint|module) ", text):
        i = m.start() + 3
        depth, j, in_str = 0, i, False
        while j < len(text):
            c = text[j]
            if in_str:
                if c == "\\":
                    j += 1
                elif c == '"':
                    in_str = False
            elif c == '"':
                in_str = True
            elif c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        spans.append((i, j + 1))
    return spans


def scratch_board(name: str) -> dict:
    src = FIX / name / "unrouted.kicad_pcb"
    rep = placer_report(src, "full")
    movable = {p["ref"] for p in rep["placement"] if p["movable"]}
    xs = [p[0] for p in rep["outline"]]
    ys = [p[1] for p in rep["outline"]]
    if not xs:  # a board without an outline (i2lcd): the centre of the box around all parts
        xs = [v for p in rep["placement"] for v in (p["box"][0], p["box"][2])]
        ys = [v for p in rep["placement"] for v in (p["box"][1], p["box"][3])]
    cx, cy = (min(xs) + max(xs)) / 2, (min(ys) + max(ys)) / 2
    text = src.read_text()
    out, pos, moved = [], 0, 0
    for a, b in footprint_spans(text):
        block = text[a:b]
        ref = re.search(r'\(fp_text reference "?([^\s")]+)"?|\(property "Reference" "([^"]*)"', block)
        ref = (ref.group(1) or ref.group(2)) if ref else None
        at = re.search(r"\n\s+\(at ([-\d.]+) ([-\d.]+)((?: [-\d.]+)?)\)", block)  # the footprint's own (at ...) comes first
        if ref in movable and at:
            block = block[:at.start(1)] + f"{cx:.4f} {cy:.4f}" + block[at.end(2):]
            moved += 1
        out.append(text[pos:a] + block)
        pos = b
    out.append(text[pos:])
    d = SDIR / name
    d.mkdir(parents=True, exist_ok=True)
    (d / "unrouted.kicad_pcb").write_text("".join(out))
    return {"board": name, "movable": len(movable), "moved": moved}


def scratch() -> int:
    boards = [line.strip() for line in (SETS / "H.txt").read_text().splitlines() if line.strip() and not line.startswith("#")]
    with cf.ThreadPoolExecutor(8) as ex:
        rows = list(ex.map(scratch_board, boards))
    bad = [r for r in rows if r["moved"] != r["movable"]]
    for r in bad:
        print(f"{r['board']}: moved {r['moved']} of {r['movable']} movable parts")
    (SETS / "S.txt").write_text("# From-scratch set S (doc 14 §3): the H boards with every movable part at the board centre.\n" +
                                "".join(r["board"] + "\n" for r in rows))
    print(f"wrote {len(rows)} boards to {SDIR}, {len(bad)} with parts not found by reference")
    return 1 if bad else 0


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("what", choices=["select", "scratch"])
    ap.add_argument("--jobs", type=int, default=6)
    a = ap.parse_args()
    raise SystemExit(select(a.jobs) if a.what == "select" else scratch())
