#!/usr/bin/env python3
"""Compares TraceMaker's DRC with KiCad's (kicad-cli pcb drc) on a set of boards.

Usage: drc_parity.py [--jobs N] [--timeout S] board.kicad_pcb ...
KiCad results are cached in build/drc/kicad/ (keyed by file path + mtime). Prints a per-board table of
violation counts for the routing-relevant types and a summary of mismatches.
"""
import argparse
import concurrent.futures as cf
import hashlib
import json
import os
import pathlib
import subprocess
import sys
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parent.parent
CACHE = ROOT / "build" / "drc" / "kicad"
TM = ROOT / "build" / "release" / "src" / "app" / "tracemaker"
TYPES = ["clearance", "shorting_items", "tracks_crossing", "track_width", "via_diameter", "annular_width",
         "drill_out_of_range", "hole_clearance", "hole_to_hole", "holes_co_located", "copper_edge_clearance",
         "items_not_allowed", "unconnected_items", "track_dangling", "via_dangling"]


def key(path: pathlib.Path) -> str:
    st = path.stat()
    return hashlib.sha1(f"{path.resolve()}:{st.st_mtime_ns}:{st.st_size}".encode()).hexdigest()[:16]


def kicad_counts(path: pathlib.Path, timeout: int) -> Counter | None:
    CACHE.mkdir(parents=True, exist_ok=True)
    out = CACHE / f"{key(path)}.json"
    if not out.exists():
        try:
            subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors",
                            "-o", str(out), str(path)], capture_output=True, timeout=timeout)
        except subprocess.TimeoutExpired:
            return None
        if not out.exists():
            return None
    d = json.loads(out.read_text())
    c = Counter(v["type"] for v in d.get("violations", []))
    c["unconnected_items"] = len(d.get("unconnected_items", []))
    return c


def tm_counts(path: pathlib.Path, timeout: int, tm: pathlib.Path = TM) -> Counter | None:
    out = ROOT / "build" / "drc" / "tm" / f"{key(path)}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    out.unlink(missing_ok=True)
    try:
        subprocess.run([str(tm), "drc", str(path), "--json", str(out)], capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        return None
    if not out.exists():
        return None
    d = json.loads(out.read_text())
    c = Counter(v["type"] for v in d.get("violations", []))
    c["unconnected_items"] = len(d.get("unconnected_items", []))
    return c


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--timeout", type=int, default=300)
    ap.add_argument("--json", help="write per-board results here")
    ap.add_argument("--tm", default=str(TM), help="tracemaker binary to compare (default: this build)")
    a = ap.parse_args()
    boards = [pathlib.Path(b) for b in a.boards if pathlib.Path(b).is_file()]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        kc = dict(zip(boards, ex.map(lambda b: kicad_counts(b, a.timeout), boards)))
        tc = dict(zip(boards, ex.map(lambda b: tm_counts(b, a.timeout, pathlib.Path(a.tm)), boards)))
    exact = 0
    per_type = Counter()
    rows = []
    for b in boards:
        k, t = kc[b], tc[b]
        if k is None or t is None:
            print(f"{'TIMEOUT/ERR':12} {b}")
            continue
        # KiCad stops reporting a violation type after 199 items (DRC_ENGINE error limit); compare capped counts.
        cap = lambda n: min(n, 199)
        diffs = {ty: (k.get(ty, 0), t.get(ty, 0)) for ty in TYPES if cap(k.get(ty, 0)) != cap(t.get(ty, 0))}
        rows.append({"board": str(b), "kicad": {ty: k.get(ty, 0) for ty in TYPES}, "ours": {ty: t.get(ty, 0) for ty in TYPES}})
        if not diffs:
            exact += 1
        for ty in diffs:
            per_type[ty] += 1
        status = "MATCH" if not diffs else "DIFF"
        detail = " ".join(f"{ty}={kv[0]}/{kv[1]}" for ty, kv in diffs.items())
        print(f"{status:6} {b.name[:48]:48} {detail}")
    print(f"\n{exact}/{len(rows)} boards match exactly on {len(TYPES)} routing-relevant types (kicad/ours shown on diffs)")
    for ty, n in per_type.most_common():
        print(f"  {ty:24} differs on {n} boards")
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps({"exact": exact, "boards": rows}, indent=1))
    return 0 if exact == len(rows) else 1


if __name__ == "__main__":
    sys.exit(main())
