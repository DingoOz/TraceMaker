#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert tscircuit's fan-out benchmark (dataset-fanout31-am62l, 72 SoC fan-out cases; doc 16 Track B) into
bench/run.py fixtures: bench/data/tscircuit/boards/fanout31/<id>/unrouted.kicad_pcb with project and problem.

  bench/prepare_fanout31.py [--inputs bench/data/tscircuit/fanout31/in] [--out bench/data/tscircuit/boards/fanout31]

The inputs are the captured constructor inputs of tscircuit's fan-out solver (`generate-dataset31.tsx` of
github.com/tscircuit/fanout-solver, MIT; the dataset repo has no licence: local use only, nothing is copied into
the repository). Each carries `simpleRouteJson` and `solverOptions`. The conversion:

- `srj-import --clearance-mm 0.08128 --outline-margin-mm 0.5 --unpointed-keepout`: the clearance is the track width the
  problem states; the outline grows so the breakout points on the bounds are not inside the board-edge rule; pads of
  the far component (RAM) are keep-outs, since the problem asks for the fan-out at the SoC only.
- plane terminations (`buses[].termination`) become zones of the net on that layer; breakout points leave on the layer
  their bus names in `connectionExitTargets`.
- layers outside `solverOptions.escapeLayers` get a rule area that forbids tracks and lets vias through.
"""
import argparse
import concurrent.futures as cf
import json
import os
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))


def convert(job):
    src, dst = job
    d = json.loads(src.read_text())
    srj = d["simpleRouteJson"]
    srj["tracemakerTrackLayers"] = d["solverOptions"]["escapeLayers"]
    dst.mkdir(parents=True, exist_ok=True)
    (dst / "problem.json").write_text(json.dumps(srj))
    cmd = [str(TM), "srj-import", str(dst / "problem.json"), "-o", str(dst / "unrouted.kicad_pcb"), "--json", str(dst / "import.json"),
           "--clearance-mm", "0.08128", "--outline-margin-mm", "0.5", "--unpointed-keepout"]
    p = subprocess.run(cmd, capture_output=True, text=True)
    return {"board": dst.name, "error": (p.stderr or p.stdout).strip()[-300:] if p.returncode else None}


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--inputs", default=str(ROOT / "bench/data/tscircuit/fanout31/in"))
    ap.add_argument("--out", default=str(ROOT / "bench/data/tscircuit/boards/fanout31"))
    ap.add_argument("--jobs", type=int, default=8)
    a = ap.parse_args()
    files = sorted(pathlib.Path(a.inputs).glob("*.json"))
    jobs = [(f, pathlib.Path(a.out) / f.stem) for f in files]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(convert, jobs))
    bad = [r for r in rows if r["error"]]
    for r in bad:
        print("FAILED", r["board"], r["error"])
    names = [r["board"] for r in rows if not r["error"]]
    sets = ROOT / "bench/sets/fanout31.txt"
    sets.write_text("# tscircuit fan-out benchmark (dataset-fanout31-am62l, 72 SoC fan-out cases), converted by bench/prepare_fanout31.py (doc 16 Track B).\n"
                    f"#fixtures: {pathlib.Path(a.out).resolve().relative_to(ROOT)}\n" + "\n".join(names) + "\n")
    print(f"{len(files)} inputs, {len(names)} converted -> {a.out}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
