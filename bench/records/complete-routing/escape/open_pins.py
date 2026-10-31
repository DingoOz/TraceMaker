#!/usr/bin/env python3
"""Why the open connections of a benchmark run are open (doc 20 §2.4): for every board of RUN_DIR, the end pins of the connections it
left open (route.json `failures`, "NET: A -> B (...)") go through `tracemaker walled` on the board's input.

usage: open_pins.py TRACEMAKER RUN_DIR > open-pins.tsv
"""
import json, os, pathlib, re, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

tm, run = sys.argv[1], pathlib.Path(sys.argv[2])


def boards():
    for line in open(run / "boards.jsonl"):
        d = json.loads(line)
        rj = run / "boards" / f"{d['board']}.kicad_pcb.route.json"
        if rj.exists():
            j = json.load(open(rj))
            pins = set()
            for f in j.get("failures", []):
                m = re.match(r"[^:]*: (\S+) -> (\S+)", f)
                if m:
                    pins |= {p for p in m.groups() if p != "zone"}
            yield d["board"], j["run"]["input"], len(j.get("failures", [])), sorted(pins)


def check(item):
    board, src, open_conns, pins = item
    if not pins:
        return f"{board}\t0\t0\t0\t0\t0\t0\t"
    with tempfile.TemporaryDirectory() as td:
        out = pathlib.Path(td) / "w.json"
        args = [tm, "walled", src, "--json", str(out)]
        for p in pins:
            args += ["--pin", p]
        r = subprocess.run(args, capture_output=True, text=True, timeout=3600)
        if r.returncode != 0:
            return f"{board}\tERROR\t{(r.stderr or r.stdout)[-200:]!r}"
        j = json.load(open(out))
    w, c, s = len(j["walled_in"]), len(j["class_only"]), len(j["soft_only"])
    detail = "; ".join(x["text"] for x in j["walled_in"] + j["soft_only"])
    return f"{board}\t{open_conns}\t{j['pins']}\t{w}\t{c}\t{s}\t{j['pins'] - w - c - s}\t{detail}"


print("board\topen_connections\tpins\twalled_in\tclass_only\tsoft_only\tcongested\twalled_and_soft_texts")
with ThreadPoolExecutor(int(os.environ.get("JOBS", "8"))) as ex:
    for line in ex.map(check, list(boards())):
        print(line, flush=True)
