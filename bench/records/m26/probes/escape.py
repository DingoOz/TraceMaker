#!/usr/bin/env python3
"""M26 F1/F2 probe: the escape plan, which never ran on srj20 (the breakout portfolio leaves it off), on the warm ramp.

Per board, three arms with the same seed and recipe, warm from the best board so far: c (control), ep (--escape-plan: dog-bone sites
and perimeter corridors, version 1), ef (--escape-flow: sites and channels by min-cost flow, version 2). The binary has F8, so no arm can
end worse than its start. One line per board in escape.jsonl.
"""
import json, os, pathlib, subprocess, sys, threading, time
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "srj20-again")); sys.argv, ARGS = sys.argv[:1], sys.argv[1:]
import again  # noqa: E402
drive = again.drive
TM = pathlib.Path(os.environ.get("TM_BIN", HERE / "tracemaker"))
import os
LIMIT, THREADS, SEED = os.environ.get("LIMIT", "1800"), os.environ.get("THREADS", "5"), os.environ.get("SEED", "501")
FRESH = os.environ.get("FRESH") == "1"
VARIANTS = os.environ.get("VARIANTS")
ARMS = [("c", []), ("ep", ["--escape-plan"]), ("ef", ["--escape-flow"])]
OUT = os.environ.get("OUT", "escape")
if OUT == "lanes":
    ARMS = [("l1", ["--package-lanes", "1"]), ("l2", ["--package-lanes", "2"]), ("l1s", ["--package-lanes", "1", "--package-lanes-mm", "4"])]
elif OUT == "shove":
    ARMS = [("vs", ["--via-shove"])]
elif OUT == "order":
    ARMS = [("o4", ["--order-all", "4"]), ("o1", ["--order-all", "1"])]
elif OUT == "guide":
    ARMS = [("pv", ["--pin-via-pen", "2"]), ("band", ["--pin-via-pen", "2", "--pin-via-mm", "1.5"]), ("vc", ["--via-cost-mm", "6"])]
if OUT == "endgame":
    ARMS = [("e", ["--endgame", "0.3"]), ("er1", ["--endgame", "0.3", "--rescue", "0.5,1,2"]),
            ("er2", ["--endgame", "0.3", "--rescue", "1,2,4", "--rescue-victims", "32"]),
            ("er3", ["--endgame", "0.5", "--rescue", "2,4,8", "--rescue-victims", "64", "--rescue-per-conn", "3"])]
if OUT == "rescue":
    ARMS = [("c", []), ("r1", ["--rescue", "0.5,1,2"]), ("r2", ["--rescue", "1,2,4", "--rescue-victims", "32"]), ("pv", ["--pin-via-pen", "2"]), ("vc", ["--via-cost-mm", "6"])]


def common():
    c = list(drive.COMMON)
    c[c.index("--threads") + 1] = THREADS
    if VARIANTS:
        c[c.index("--variants") + 1] = VARIANTS
    for k in ("--time", "--time-total"):
        c[c.index(k) + 1] = LIMIT
    return c


def run_board(b, port0, lock):
    op, pcb, what = again.pool(b)[0]
    d = HERE / OUT / b
    procs = {}
    for k, (name, extra) in enumerate(ARMS):
        a = d / name
        a.mkdir(parents=True, exist_ok=True)
        (a / "out.kicad_pro").write_bytes((drive.FIX / b / "unrouted.kicad_pro").read_bytes())
        cmd = [str(TM), "route", str(drive.FIX / b / "unrouted.kicad_pcb"), "-o", str(a / "out.kicad_pcb"), "--json", str(a / "out.json"),
               *common(), *extra, "--seed", SEED, *([] if FRESH else ["--warm-pcb", pcb]), "--view", "--view-port", str(port0 + k)]
        (a / "cmd.txt").write_text(" ".join(cmd) + ("\n# start: nothing\n" if FRESH else f"\n# start: {what}, {op} open\n"))
        procs[name] = subprocess.Popen(cmd, stdout=open(a / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    t0 = time.time()
    for p in procs.values():
        p.wait()
    res = {"board": b, "start": what, "start_open": op, "seconds": round(time.time() - t0), "arms": {}}
    for name, _ in ARMS:
        try:
            j = json.load(open(d / name / "out.json"))
            res["arms"][name] = {"endgame_tried": j.get("endgame_tried"), "open": j["connections"] - j["routed"], "escape_corridors": j.get("escape_corridors"), "vias": j.get("vias"), "rescued": j.get("rescued"), "rescue_tried": j.get("rescue_tried")}
        except (OSError, ValueError, KeyError):
            res["arms"][name] = {"open": None}
    done = {n: a["open"] for n, a in res["arms"].items() if a["open"] is not None}
    if done:
        best = min(done, key=lambda n: (done[n], n))
        res.update({"best_arm": best, **drive.judge(b, d / best / "out.kicad_pcb")})
    with lock:
        with open(HERE / f"{OUT}.jsonl", "a") as f:
            f.write(json.dumps(res) + "\n")
        print(json.dumps(res), flush=True)


if __name__ == "__main__":
    lock = threading.Lock()
    ts = [threading.Thread(target=run_board, args=(b, 9001 + 10 * i + {'escape': 0, 'rescue': 100, 'endgame': 200, 'fresh': 300, 'order': 500, 'lanes': 600, 'shove': 700}.get(OUT, 400), lock)) for i, b in enumerate(ARGS)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
