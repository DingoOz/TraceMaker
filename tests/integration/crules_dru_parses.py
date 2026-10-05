#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The generated sidecar .kicad_dru must be valid KiCad syntax: kicad-cli silently ignores a whole rules file with a
syntax error (checked with KiCad 10), so every generated rule would vanish without a message. Sentinel check: append
a rule that must fire (5 mm clearance) to the generated file, place it next to a board as <board>.kicad_dru, and
require KiCad's violation count to rise. Skips (77) without kicad-cli or the fixtures.

  crules_dru_parses.py TRACEMAKER WORKDIR
"""
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def violations(board: pathlib.Path) -> int:
    out = board.with_suffix(".drc.json")
    subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "-o", str(out), str(board)], capture_output=True, timeout=600)
    return len(json.loads(out.read_text())["violations"])


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    if not shutil.which("kicad-cli"):
        print("kicad-cli missing: skip")
        return 77
    boards = ["1Bitsy_1bitsy", "ArduinoDueClone_ATSAM3X8EA"]
    if not all((FIX / b / "unrouted.kicad_pcb").exists() for b in boards):
        print("fixtures missing: skip")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    bad = 0
    for b in boards:
        board = work / f"{b}.kicad_pcb"
        shutil.copy(FIX / b / "unrouted.kicad_pcb", board)
        dru = board.with_suffix(".kicad_dru")
        dru.unlink(missing_ok=True)
        base = violations(board)
        side = work / f"{b}.side.kicad_dru"
        subprocess.run([str(tm), "rules", str(FIX / b / "unrouted.kicad_pcb"), "--mode", "on", "--dru", str(side)], capture_output=True, check=True)
        text = side.read_text()
        if "(rule " not in text:
            print(f"{b}: no generated rule (detection changed?)")
            bad += 1
            continue
        dru.write_text(text + '\n(rule "sentinel" (constraint clearance (min 5mm)))\n')
        with_rules = violations(board)
        dru.unlink()
        ok = with_rules > base
        print(f"{b}: {base} violations without rules, {with_rules} with the generated rules + sentinel: {'parsed' if ok else 'IGNORED BY KICAD'}")
        bad += not ok
    return 1 if bad else 0


if __name__ == "__main__":
    raise SystemExit(main())
