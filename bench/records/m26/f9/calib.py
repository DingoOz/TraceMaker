#!/usr/bin/env python3
"""M26 F9 calibration: the router's line_crossings per connection on every srj20 board (a 1-variant run with a tiny budget)."""
import concurrent.futures as cf, json, pathlib, subprocess
HERE = pathlib.Path(__file__).resolve().parent
TM = HERE.parents[3] / "build/release/src/app/tracemaker"
SET = pathlib.Path("/home/dingo/Programming/TraceMaker/bench/data/tscircuit/boards/srj20")

def one(i, d):
    j = HERE / "calib" / f"{d.name}.json"
    j.parent.mkdir(exist_ok=True)
    subprocess.run([str(TM), "route", str(d / "unrouted.kicad_pcb"), "-o", str(j.with_suffix(".kicad_pcb")), "--json", str(j), "--variants", "1",
                    "--threads", "1", "--work", "1000", "--no-kb", "--view", "--view-port", str(9700 + i)], capture_output=True)
    s = json.load(open(j))
    return d.name, s["line_crossings"], s["connections"]

boards = sorted(p for p in SET.iterdir() if (p / "unrouted.kicad_pcb").exists())
with cf.ThreadPoolExecutor(8) as ex:
    rows = list(ex.map(lambda a: one(*a), enumerate(boards)))
json.dump(rows, open(HERE / "calib.json", "w"))
