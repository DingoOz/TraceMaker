#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The live page's status files (D165): a tracemaker process writes <TM_LIVE_DIR>/<pid>.json while it runs and its final state
when it ends, and the progress site's feed (devsite/livefeed.py) reads it.

  live_status.py <tracemaker> <work dir>

A `session` routes a test board and then waits on standard input, so the process lives past the writer's first second.
Checked: the file appears while the process runs, says running, and has the router's slot with the routed count; after stdin
closes it says done with exit 0; the feed lists the process while it runs and then as done; TM_LIVE=0 writes nothing.
"""
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "tests/boards/plane_smd/plane_smd.kicad_pcb"
sys.path.insert(0, str(ROOT / "devsite"))


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)


def wait_for(fn, seconds=20.0):
    t = time.time() + seconds
    while time.time() < t:
        v = fn()
        if v:
            return v
        time.sleep(0.2)
    return None


def main():
    tm, work = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    live = work / "live"
    live.mkdir(parents=True)
    os.environ["TM_LIVE_DIR"] = str(live)
    import livefeed  # reads TM_LIVE_DIR at each snapshot

    req = json.dumps({"tool": "route", "args": {"work": 200000}}) + "\n"
    p = subprocess.Popen([str(tm), "session", str(BOARD), "--jsonl"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True)
    p.stdin.write(req)
    p.stdin.flush()
    check(p.stdout.readline().startswith("{"), "the session answered the route")
    f = live / f"{p.pid}.json"

    def running():
        try:
            d = json.loads(f.read_text())
        except (OSError, ValueError):
            return None
        return d if d.get("slots") else None

    d = wait_for(running)
    check(d is not None, f"{f.name} written with a router slot while the process runs")
    check(d["state"] == "running" and d["exit"] is None, "state running")
    check(d["argv"][1] == "session" and d["pid"] == p.pid, "command line and pid")
    s = d["slots"][0]
    check(s["total"] > 0 and 0 <= s["routed"] <= s["total"] and s["runs"] >= 1, f"slot counts {s}")
    snap = livefeed.snapshot()
    mine = [q for q in snap["procs"] if q["pid"] == p.pid]
    check(len(mine) == 1 and mine[0]["state"] == "running" and "proc" in mine[0], "the feed lists the running process with /proc data")
    p.stdin.close()
    check(p.wait(timeout=60) == 0, "session exit 0")
    d = json.loads(f.read_text())
    check(d["state"] == "done" and d["exit"] == 0, f"final state done, exit 0 (got {d['state']}, {d['exit']})")
    check(not (live / f"{p.pid}.json.tmp").exists(), "no temporary file left")
    mine = [q for q in livefeed.snapshot()["procs"] if q["pid"] == p.pid]
    check(len(mine) == 1 and mine[0]["state"] == "done", "the feed lists it as done")

    env = {**os.environ, "TM_LIVE": "0"}
    q = subprocess.Popen([str(tm), "session", str(BOARD), "--jsonl"], stdin=subprocess.PIPE, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, env=env)
    time.sleep(2.5)
    q.stdin.close()
    q.wait(timeout=60)
    check(not (live / f"{q.pid}.json").exists(), "TM_LIVE=0 writes nothing")
    print("live status: ok")


if __name__ == "__main__":
    main()
