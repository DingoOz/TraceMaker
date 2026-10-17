#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""prep_board.py PCBench-board-name LAYERS: makes reports/realworld/boards/NAME/ for the closure loop and the page: planes.kicad_pcb (the unrouted board plus the
designer's pours), its KiCad DRC (planes.drc.json, the baseline), and human/ (the designer's own routing judged the same way)."""
import pathlib, shutil, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import ledger, rules

ROOT = pathlib.Path(__file__).resolve().parents[3]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def main() -> int:
    name, layers = sys.argv[1], sys.argv[2]
    B = ROOT / "reports/realworld/boards" / name
    (B / "human").mkdir(parents=True, exist_ok=True)
    unrouted = (FIX / name / "unrouted.kicad_pcb").read_text()
    raw = (FIX / name / "raw.kicad_pcb").read_text(errors="replace")
    (B / "planes.kicad_pcb").write_text(rules.with_designer_zones(unrouted, raw))
    for stem_src, dst in (("unrouted.kicad_prl", "planes.kicad_prl"), ("raw.kicad_prl", "human/routed.kicad_prl")):
        if (FIX / name / stem_src).exists():
            shutil.copy(FIX / name / stem_src, B / dst)
    shutil.copy(FIX / name / "raw.kicad_pcb", B / "human/routed.kicad_pcb")
    subprocess.run([sys.executable, str(pathlib.Path(__file__).parent / "ledger.py"), str(B / "human/routed.kicad_pcb"), str(B / "planes.kicad_pcb"), str(B / "human/ledger.json"),
                    "--layers", layers], stdout=open(B / "human/ledger.txt", "w"), stderr=subprocess.STDOUT)
    print(name, (B / "human/ledger.txt").read_text().splitlines()[0])
    return 0


if __name__ == "__main__":
    sys.exit(main())
