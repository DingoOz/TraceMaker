#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""recover_run.py BOARD RUN: a run that was killed before its final write (time limit of a batch) has no routed.kicad_pcb. This picks the judged state
(a subdirectory with routed.kicad_pcb and routed.drc.json) with the fewest open connections, then the fewest violations, and copies it to RUN/routed.kicad_pcb
so that closure.py --resume RUN can finish it. The tiers in closure.json are those of the end of the killed run, not of the recovered state (an approximation)."""
import json
import pathlib
import sys

RW = pathlib.Path(__file__).resolve().parents[1]


def main() -> int:
    board, run = sys.argv[1].removesuffix("+planes"), sys.argv[2]
    d = RW / "boards" / board / run
    if (d / "routed.kicad_pcb").exists():
        print(f"{board} {run}: already has routed.kicad_pcb")
        return 0
    best = None
    for p in sorted(d.glob("*/routed.drc.json")):
        if not (p.parent / "routed.kicad_pcb").exists():
            continue
        drc = json.loads(p.read_text())
        viol = sum(1 for v in drc.get("violations", []))
        key = (len(drc.get("unconnected_items", [])), viol, p.parent.name)
        if best is None or key < best[0]:
            best = (key, p.parent)
    if best is None:
        print(f"{board} {run}: nothing to recover")
        return 1
    (d / "routed.kicad_pcb").write_text((best[1] / "routed.kicad_pcb").read_text())
    print(f"{board} {run}: recovered {best[1].name} ({best[0][0]} open, {best[0][1]} violations)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
