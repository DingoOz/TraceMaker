#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Roadmap M12 gate for length tuning: a net with a custom `length` rule ends inside its range, judged by KiCad.

  length_tuning.py <tracemaker> <workdir>

Routes a small PCBench board with a .kicad_dru that asks IN2 for 34-36 mm (it routes at about 27 mm untuned),
then runs kicad-cli DRC: no length_out_of_range and no router-introduced errors. Exit 77 when the fixture or
kicad-cli is missing.
"""
import json
import math
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
SRC = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench/kitspace_hbridge_driver/unrouted.kicad_pcb"
RULE = """(version 1)
(rule "IN2_length"
\t(constraint length (min 34mm) (max 36mm) (opt 35mm))
\t(condition "A.NetName == 'IN2'"))
"""


def main() -> int:
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    if not SRC.exists() or not shutil.which("kicad-cli"):
        print("fixture or kicad-cli missing: skipped")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    board = work / "lt.kicad_pcb"
    shutil.copy(SRC, board)
    for stem in ("lt", "lt_routed"):
        (work / f"{stem}.kicad_dru").write_text(RULE)
        (work / f"{stem}.kicad_pro").write_text(json.dumps({"meta": {"filename": f"{stem}.kicad_pro", "version": 1}}))
    out = work / "lt_routed.kicad_pcb"
    subprocess.run([tm, "route", str(board), "-o", str(out), "--work", "3000000", "--threads", "4", "--variants", "4", "--no-kb"], check=True, capture_output=True)
    js = work / "lt.json"
    subprocess.run([tm, "inspect", str(out), "--json", str(js)], check=True, capture_output=True)
    d = json.loads(js.read_text())
    length = sum(math.hypot(t["ex"] - t["sx"], t["ey"] - t["sy"]) for t in d["tracks"] if t["net"] == "IN2") / 1e6
    drc = work / "drc.json"
    subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", str(drc), str(out)], capture_output=True)
    v = json.loads(drc.read_text())
    bad = [x for x in v["violations"] if x["type"] == "length_out_of_range" or (x["severity"] == "error" and any(
        i["description"].startswith(("Track", "Via")) for i in x["items"]))]
    print(f"IN2 length {length:.2f} mm (rule 34-36 mm); unconnected {len(v['unconnected_items'])}; length/router errors {len(bad)}")
    return 0 if 34.0 <= length <= 36.0 and not bad and not v["unconnected_items"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
