#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Router speed A/B between two engine binaries on the same boards and work budget (doc 05 §28).

  bench/speed_ab.py BEFORE_BINARY AFTER_BINARY [--work 10000000] [--boards ...] [--demos [DIR/NAME ...]] [--gpu]

Each board is routed by one portfolio variant on one thread with a deterministic work budget and, unless --gpu,
CPU fields, so both binaries do the same search and the difference is the cost per unit of work. Runs are
sequential to keep timings clean. Reports wall and CPU seconds, peak memory and, where /usr/bin/time reports it
(macOS), instructions retired; and whether the routed boards are byte-identical. Not a replacement for the
KiCad-judged tiers (bench/run.py).

PCBench boards (default) come without project files, so they route on KiCad's default rules. --demos instead
routes KiCad's demo projects (scripts/fetch_fixtures.sh kicad-demos) with their tracks and vias removed and their
.kicad_pro / .kicad_dru kept: net classes with name patterns and custom rules.
"""
import argparse
import hashlib
import json
import pathlib
import platform
import re
import shutil
import subprocess

from prepare_dac2020 import strip_routing

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
BOARDS = ["oskirby_logicbone", "decelerator4030_decelerator4030", "kitspace_EEZ%20DIB%20MCU%20r1B2", "sbc_sbc",
          "front-end-modules_LimeSDR_Sony", "Aleste-520EX_aleste", "CoreOne-xCORE200-Original_CoreOne", "LeeChee_1800",
          "MonApollo_analog-board", "KiCad-Library_Teensy_test_layout"]
# Small demo projects whose net classes use name patterns (vme-wren and jetson-agx-thor also have custom rules but
# spend minutes in set-up before the first route).
DEMOS = ["complex_hierarchy/complex_hierarchy", "pic_programmer/pic_programmer", "interf_u/interf_u",
         "kit-dev-coldfire-xilinx_5213/kit-dev-coldfire-xilinx_5213", "cm5_minima/CM5_MINIMA_3", "tiny_tapeout/tinytapeout-demo"]
TIME_FLAG = "-l" if platform.system() == "Darwin" else "-v"


def prepare_demo(demo: str, out: pathlib.Path) -> pathlib.Path:
    """Unrouted copy of a KiCad demo board next to copies of its project and custom rules."""
    src = ROOT / "bench/data/kicad/demos" / demo
    dst = out / "demos" / src.name / src.name
    dst.parent.mkdir(parents=True, exist_ok=True)
    for ext in (".kicad_pro", ".kicad_dru"):
        if src.with_suffix(ext).exists():
            shutil.copyfile(src.with_suffix(ext), dst.with_suffix(ext))
    pcb = dst.with_suffix(".kicad_pcb")
    pcb.write_text(strip_routing(src.with_suffix(".kicad_pcb").read_text()))
    return pcb


def run(binary: str, board: pathlib.Path, work: int, gpu: bool, out: pathlib.Path) -> dict:
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = ["/usr/bin/time", TIME_FLAG, binary, "route", str(board), "-o", str(out), "--work",
           str(work), "--time", "3600", "--threads", "1", "--variants", "1", "--no-kb"] + ([] if gpu else ["--no-gpu"])
    p = subprocess.run(cmd, capture_output=True, text=True)
    text = p.stdout + p.stderr

    def num(pattern: str) -> float | None:
        m = re.search(pattern, text, re.M)
        return float(m.group(1)) if m else None

    routed = re.search(r"routed (\d+)/(\d+) connections", text)
    if platform.system() == "Darwin":
        wall, user, sys_ = num(r"([\d.]+) real"), num(r"([\d.]+) user"), num(r"([\d.]+) sys")
        rss_mb, instr = (num(r"^\s*(\d+)\s+maximum resident set size") or 0) / 2**20, num(r"^\s*(\d+)\s+instructions retired")
    else:
        m = re.search(r"Elapsed \(wall clock\) time.*: (?:(\d+):)?(\d+):([\d.]+)", text)
        wall = (int(m.group(1) or 0) * 3600 + int(m.group(2)) * 60 + float(m.group(3))) if m else None
        user, sys_ = num(r"User time \(seconds\): ([\d.]+)"), num(r"System time \(seconds\): ([\d.]+)")
        rss_mb, instr = (num(r"Maximum resident set size \(kbytes\): (\d+)") or 0) / 1024, None
    return {"ok": p.returncode == 0 and out.exists(), "routed": int(routed.group(1)) if routed else -1,
            "connections": int(routed.group(2)) if routed else -1, "wall": wall, "cpu": (user or 0) + (sys_ or 0),
            "rss_mb": rss_mb, "instructions": instr,
            "md5": hashlib.md5(out.read_bytes()).hexdigest() if out.exists() else None}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--work", type=int, default=10_000_000)
    ap.add_argument("--boards", nargs="*", default=BOARDS, help="PCBench board names")
    ap.add_argument("--demos", nargs="*", help="route these KiCad demo projects (default: all of DEMOS) instead of PCBench")
    ap.add_argument("--gpu", action="store_true", help="let both binaries use their GPU backend for fields")
    ap.add_argument("--out", default=str(ROOT / "build/speed_ab"))
    a = ap.parse_args()
    out = pathlib.Path(a.out)
    inputs = ([(pathlib.Path(d).name, prepare_demo(d, out)) for d in (a.demos or DEMOS)] if a.demos is not None else
              [(b, FIX / b / "unrouted.kicad_pcb") for b in a.boards])
    rows = []
    for b, board in inputs:
        r = {"board": b}
        for side, binary in (("before", a.before), ("after", a.after)):
            r[side] = run(binary, board, a.work, a.gpu, out / side / f"{b}.kicad_pcb")
        r["identical"] = r["before"]["md5"] is not None and r["before"]["md5"] == r["after"]["md5"]
        rows.append(r)
        x, y = r["before"], r["after"]
        print(f"{b}: {x['wall']:.1f} -> {y['wall']:.1f} s, routed {x['routed']} -> {y['routed']}/{y['connections']}, "
              f"{'identical' if r['identical'] else 'DIFFERENT'}", flush=True)
    (out / "summary.json").write_text(json.dumps({"work": a.work, "gpu": a.gpu, "rows": rows}, indent=1))

    def ratio(key: str, r: dict) -> str:
        x, y = r["before"][key], r["after"][key]
        return f"{x / y:.2f}x" if x and y else "–"

    print(f"\n| Board | Routed | Wall s before → after | CPU s before → after | Speed-up (CPU) | Instructions | "
          f"Peak MB before → after | Output |")
    print("|---|--:|--:|--:|--:|--:|--:|---|")
    tot = {"before": [0.0, 0.0, 0.0], "after": [0.0, 0.0, 0.0]}
    for r in rows:
        x, y = r["before"], r["after"]
        for side in ("before", "after"):
            tot[side][0] += r[side]["wall"] or 0
            tot[side][1] += r[side]["cpu"]
            tot[side][2] += r[side]["instructions"] or 0
        instr = f"{x['instructions'] / 1e9:.0f} G → {y['instructions'] / 1e9:.0f} G" if x["instructions"] and y["instructions"] else "–"
        print(f"| {r['board']} | {y['routed']}/{y['connections']} | {x['wall']:.1f} → {y['wall']:.1f} | {x['cpu']:.1f} → "
              f"{y['cpu']:.1f} | {ratio('cpu', r)} | {instr} | {x['rss_mb']:.0f} → {y['rss_mb']:.0f} | "
              f"{'identical' if r['identical'] else 'different'} |")
    tb, ta = tot["before"], tot["after"]
    instr = f"{tb[2] / 1e9:.0f} G → {ta[2] / 1e9:.0f} G" if tb[2] and ta[2] else "–"
    print(f"| **Total** | | {tb[0]:.1f} → {ta[0]:.1f} | {tb[1]:.1f} → {ta[1]:.1f} | "
          f"{tb[1] / ta[1] if ta[1] else 0:.2f}x | {instr} | | {sum(r['identical'] for r in rows)}/{len(rows)} identical |")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
