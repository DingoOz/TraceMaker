#!/usr/bin/env python3
"""False-alarm sweep for `tracemaker walled` (doc 20 §2.4): on every board a benchmark run left KiCad-clean (no error added,
nothing unconnected), every pin was connected under the rules, so a pin of its input that `walled` proves enclosed is a bug in
the proof. Pins walled in only by zone fills or net-less copper graphics (the router's settings) are counted apart.

usage: walled_sweep.py TRACEMAKER RUN_DIR [RUN_DIR ...] > sweep.tsv
"""
import json, os, pathlib, subprocess, sys, tempfile, time
from concurrent.futures import ThreadPoolExecutor

tm = sys.argv[1]


def clean_boards(run):
    run = pathlib.Path(run)
    for line in open(run / "boards.jsonl"):
        d = json.loads(line)
        if d.get("exit") == 0 and not d.get("added_errors") and d.get("unconnected_after") == 0 and d.get("routed") == d.get("connections"):
            rj = run / "boards" / f"{d['board']}.kicad_pcb.route.json"
            if rj.exists():
                yield run.name, d["board"], json.load(open(rj))["run"]["input"]


def check(item):
    run, board, src = item
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / "w.json"
        t = time.time()
        r = subprocess.run([tm, "walled", src, "--json", str(out)], capture_output=True, text=True, timeout=1800)
        dt = time.time() - t
        if r.returncode != 0:
            return f"{run}\t{board}\tERROR\t{(r.stderr or r.stdout)[-200:]!r}"
        j = json.load(open(out))
    pins = ",".join(w["pin"] for w in j["walled_in"])
    return f"{run}\t{board}\t{j['pins']}\t{len(j['walled_in'])}\t{len(j['soft_only'])}\t{dt:.1f}\t{pins}"


items = [x for run in sys.argv[2:] for x in clean_boards(run)]
print("run\tboard\tpins\twalled_in\tsoft_only\tseconds\twalled_pins")
with ThreadPoolExecutor(int(os.environ.get("JOBS", "12"))) as ex:
    for line in ex.map(check, items):
        print(line, flush=True)
