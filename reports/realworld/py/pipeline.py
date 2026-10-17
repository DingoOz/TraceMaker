#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The relaxation as ONE step: route under escalating rules, judge with KiCad, SIMULATE what was relaxed. A board counts as routed only if all three hold.

  pipeline.py BOARD[+planes] NAME STAGES [ladder options...]      e.g.  pipeline.py DronPi_dronPi+planes ladder-K T0:20000000,T1:20000000 --keep-power
  pipeline.py BOARD[+planes] closure-N [closure options...]       step 1 is the closure loop (closure.py: relax the nets in the way, power last; move parts)

 1. ladder.py   the rule-escalation ladder (T0 = the board's own rules; each stage starts from the copper of the one before); --keep-power / --keep-supply
                leave the power nets on the board's own rules
 2. ledger.py   KiCad 10 DRC of the routed copper under the board's own rules: open connections and every compromise
 3. safety.py   ngspice crosstalk of every compromised signal pair, IPC-2221 + DC drop of narrowed copper, via impedance (in the report image)

Exit status 0 only when KiCad reports 0 open connections AND the safety verdict is not FAIL. The result files are the ones the development site shows
(boards/BOARD/NAME/ledger.json, sim/results/BOARD/safety.NAME.json).
"""
import json
import pathlib
import re
import subprocess
import sys

RW = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    board_arg, name = sys.argv[1], sys.argv[2]
    closing = name.startswith("closure-")
    stages = "" if closing else sys.argv[3]
    extra = sys.argv[3:] if closing else sys.argv[4:]
    board = board_arg.removesuffix("+planes")
    py = RW / "py"
    r = subprocess.run([sys.executable, str(py / ("closure.py" if closing else "ladder.py")), board_arg, name, *([] if closing else [stages]), *extra])
    if closing and r.returncode == 1:
        r.returncode = 0  # closure stopped with open connections: still judge and simulate the best state it reached
    run_dir = RW / "boards" / board / name
    if r.returncode or not (run_dir / "routed.kicad_pcb").exists():
        print(f"{board} {name}: ladder failed")
        return 2
    base = RW / "boards" / board / "planes.kicad_pcb" if board_arg.endswith("+planes") else None
    if base is None:
        base = pathlib.Path(RW.parents[1] / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench" / board / "unrouted.kicad_pcb")
    if (run_dir / "baseline.kicad_pcb").exists():
        base = run_dir / "baseline.kicad_pcb"  # the unrouted board with the parts where the closure loop moved them
    layers = len(re.findall(r"\(\d+ (?:F|B|In\d+)\.Cu ", base.read_text())) or 2
    subprocess.run([sys.executable, str(py / "ledger.py"), str(run_dir / "routed.kicad_pcb"), str(base), str(run_dir / "ledger.json"), "--layers", str(layers)],
                   stdout=open(run_dir / "ledger.log", "w"), stderr=subprocess.STDOUT)
    led = json.loads((run_dir / "ledger.json").read_text())
    subprocess.run([str(RW / "dk.sh"), "python3", "py/safety.py", board, name], cwd=RW)
    saf = json.loads((RW / "sim/results" / board / f"safety.{name}.json").read_text())
    ok = led["unconnected"] == 0 and saf["verdict"] != "FAIL"
    print(f"{board} {name}: KiCad open {led['unconnected']}, compromises {len(led['added'])}, safety {saf['verdict']} -> {'ROUTED AND SAFE' if ok else 'NOT ACCEPTED'}")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
