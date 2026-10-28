#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Time-domain matching (doc 05 §59, D147) on a generated board: delay rules in picoseconds on a net class with a tuning profile.

  delay_tuning.py <tracemaker> <work dir>

Generates bench/make_delay_boards.py's ddr8 (a byte lane from a front controller to a back memory: a 205-215 ps window on every
lane, at most 3 ps of skew across them and 0.5 ps within the strobe pair), routes it with a work budget, and judges the written
board with TraceMaker's DRC, which measures delays as KiCad 10.0.6 does: every lane routed, no length or skew violation. The same
board with the rules removed shows that the window is not met by routing alone. With kicad-cli present, KiCad's DRC judges the
tuned board too. Needs no fixtures and no GPU.
"""
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
TIMING = ("length_out_of_range", "skew_out_of_range")


def run(cmd, ok=(0,)):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if p.returncode not in ok:
        print(p.stdout[-3000:], p.stderr[-3000:])
        raise SystemExit(f"FAIL: {' '.join(str(c) for c in cmd[:3])} exited {p.returncode}")
    return p


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    run([sys.executable, ROOT / "bench/make_delay_boards.py", work, "ddr8"])
    src = work / "ddr8/ddr8.kicad_pcb"
    fails = []

    def check(cond, text):
        if not cond:
            fails.append(text)

    def route(tag, rules=True):
        # The router reads the rules next to its input, so each run routes its own copy.
        inp = src.with_name(f"ddr8.{tag}-in.kicad_pcb")
        out = src.with_name(f"ddr8.{tag}.kicad_pcb")
        for f in (inp, out):
            f.with_suffix(".kicad_pro").write_bytes(src.with_suffix(".kicad_pro").read_bytes())
            f.with_suffix(".kicad_dru").unlink(missing_ok=True)
        inp.write_bytes(src.read_bytes())
        if rules:
            inp.with_suffix(".kicad_dru").write_bytes(src.with_suffix(".kicad_dru").read_bytes())
        out.unlink(missing_ok=True)
        js = out.with_suffix(".json")
        run([tm, "route", inp, "-o", out, "--work", 3_000_000, "--time", "3600", "--no-kb", "--no-gpu", "--json", js], ok=(0, 3))
        return out, json.loads(js.read_text())

    def timing(board):
        # Judge with the rules, whichever way the board was routed.
        board.with_suffix(".kicad_dru").write_bytes(src.with_suffix(".kicad_dru").read_bytes())
        js = board.with_suffix(".drc.json")
        subprocess.run([str(tm), "drc", str(board), "--json", str(js)], capture_output=True)  # exits non-zero on violations
        return [v["description"] for v in json.loads(js.read_text())["violations"] if v["type"] in TIMING]

    out, s = route("tuned")
    found = timing(out)
    print(f"ddr8 with the rules: routed {s['routed']}/{s['connections']}, {len(found)} timing violation(s)")
    for d in found[:5]:
        print("   " + d)
    check(s["routed"] == s["connections"], f"tuned: routed {s['routed']}/{s['connections']}")
    check(not found, f"tuned: {len(found)} length or skew violation(s)")
    plain, sp = route("plain", rules=False)
    missed = timing(plain)
    print(f"ddr8 without the rules: routed {sp['routed']}/{sp['connections']}, {len(missed)} timing violation(s) against them")
    check(any("length" in d for d in missed), "plain: the window is met without tuning; the fixture no longer tests anything")
    if shutil.which("kicad-cli"):
        drc = out.with_suffix(".kicad.json")
        subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", str(drc), str(out)], capture_output=True)
        kicad = [v["description"] for v in json.loads(drc.read_text())["violations"] if v["type"] in TIMING]
        print(f"KiCad: {len(kicad)} timing violation(s) on the tuned board")
        check(not kicad, f"KiCad: {kicad[:2]}")
    for f in fails:
        print("FAIL: " + f)
    print("PASS" if not fails else f"{len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
