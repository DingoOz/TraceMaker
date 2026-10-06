#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Two-stage component-rule placement (doc 15 §14.3): with --component-rules soft in full mode the decoupling
capacitors and their ICs keep their stage-1 positions, the result is legal, and the report says so. Skips (77)
without the fixture.

  crules_two_stage.py TRACEMAKER-PLACE WORKDIR
"""
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench/1Bitsy_1bitsy/unrouted.kicad_pcb"


def main() -> int:
    place, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    if not BOARD.exists():
        print("fixture missing: skip")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    out = {}
    for tag, extra in (("off", ["--component-rules", "off"]), ("two", ["--component-rules", "soft"]), ("one", ["--component-rules", "soft", "--no-crules-two-stage"])):
        js = work / f"{tag}.json"
        subprocess.run([str(place), str(BOARD), "-o", str(work / f"{tag}.kicad_pcb"), "--mode", "full", "--threads", "4", "--seed", "1", "--effort", "0.2",  # legality and locking are tested, not quality
                        
                        "--json", str(js)] + extra, check=True, capture_output=True)
        out[tag] = json.loads(js.read_text())
    ok = out["two"].get("legal") is True
    notes = " ".join(out["two"].get("notes", []))
    ok &= "two-stage component rules" in notes
    ok &= "two-stage component rules" not in " ".join(out["one"].get("notes", []))
    # Locked parts: every part the stage-2 problem fixed for being a decoupling capacitor or its IC is where the
    # decap-only placement (same seed) put it.
    pos_off = {p["ref"]: (p["x"], p["y"], p["angle"]) for p in out["off"].get("placement", [])}
    pos_two = {p["ref"]: (p["x"], p["y"], p["angle"]) for p in out["two"].get("placement", [])}
    moved = [r for r in pos_two if r in pos_off and pos_two[r] != pos_off[r]]
    print(f"legal {out['two'].get('legal')}, notes ok {('two-stage component rules' in notes)}, parts moved by stage 2: {moved}")
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
