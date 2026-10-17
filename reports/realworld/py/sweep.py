#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Route a board under a rule tier: sweep.py BOARD TIER WORK [extra route options...]  ->  reports/realworld/boards/BOARD/TIER-WORK[-tag]/"""
import pathlib
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import rules

ROOT = pathlib.Path(__file__).resolve().parents[3]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = ROOT / "build/release/src/app/tracemaker"


def main() -> int:
    board, tier, work = sys.argv[1], sys.argv[2], sys.argv[3]
    extra = sys.argv[4:]
    planes = board.endswith("+planes")  # the designer's own GND zones put back (the fixture drops them)
    board = board.removesuffix("+planes")
    if planes:
        extra = ["--soft-zones"] + extra
    tag = ("-" + "".join(c for c in "_".join(extra) if c.isalnum() or c == "_")) if extra else ""
    out = ROOT / "reports/realworld/boards" / board / f"{'planes-' if planes else ''}{tier}-{work}{tag}"
    out.mkdir(parents=True, exist_ok=True)
    src = ((ROOT / "reports/realworld/boards" / board / "planes.kicad_pcb") if planes else (FIX / board / "unrouted.kicad_pcb")).read_text()
    (out / "relaxed.kicad_pcb").write_text(rules.apply(src, rules.TIERS[tier]))
    cmd = [str(TM), "route", str(out / "relaxed.kicad_pcb"), "-o", str(out / "routed_relaxed.kicad_pcb"), "--work", work, "--threads", "8", "--no-gpu", "--no-kb"] + extra
    with open(out / "route.log", "w") as f:
        subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    routed = (out / "routed_relaxed.kicad_pcb").read_text()
    (out / "routed.kicad_pcb").write_text(rules.apply(routed, rules.TIERS["T0"]))  # same copper, the board's own rules back
    line = next((l for l in (out / "route.log").read_text().splitlines() if l.startswith("routed ")), "no result")
    print(f"{board[:28]:28s} {'planes' if planes else 'fixture'} {tier} {work:>9s} {tag:12s} {line[:110]}", flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
