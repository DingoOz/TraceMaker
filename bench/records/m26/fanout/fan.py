#!/usr/bin/env python3
"""M26 fan-out first: route each board from its dog-bone fan-out (fanout.py), same recipe as the M26 control from nothing
(3,300 s, 16 variants, 6 threads, seed 403; control 152 7, 070 7, 040 6 open). Arms: k1 (outer ring escapes on F.Cu), k2 (outer two
rings), k0 (every ball). One line per board and arm in fan.jsonl. usage: fan.py ARM board...
"""
import json, os, pathlib, subprocess, sys, threading, time
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "srj20-never")); sys.argv, ARGS = sys.argv[:1], sys.argv[1:]
import drive  # noqa: E402
TM = HERE / "tracemaker"
LIMIT, THREADS, SEED = os.environ.get("LIMIT", "3300"), os.environ.get("THREADS", "6"), os.environ.get("SEED", "403")
ARM, BOARDS = ARGS[0], ARGS[1:]
SKIP = int(ARM[1:])


def common():
    c = list(drive.COMMON)
    c[c.index("--threads") + 1] = THREADS
    for k in ("--time", "--time-total"):
        c[c.index(k) + 1] = LIMIT
    return c


def run_board(b, port, lock):
    d = HERE / b / ARM
    subprocess.run([sys.executable, str(HERE / "fanout.py"), str(drive.FIX / b / "unrouted.kicad_pcb"), str(d), str(SKIP)], check=True)
    (d / "out.kicad_pro").write_bytes((drive.FIX / b / "unrouted.kicad_pro").read_bytes())
    cmd = [str(TM), "route", str(d / "board.kicad_pcb"), "-o", str(d / "out.kicad_pcb"), "--json", str(d / "out.json"), *common(),
           "--seed", SEED, "--view", "--view-port", str(port)]
    (d / "cmd.txt").write_text(" ".join(cmd) + "\n")
    t0 = time.time()
    subprocess.run(cmd, stdout=open(d / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    res = {"board": b, "arm": ARM, "seconds": round(time.time() - t0)}
    try:
        j = json.load(open(d / "out.json"))
        res.update({"open": j["connections"] - j["routed"], "vias": j.get("vias")})
        res.update(drive.judge(b, d / "out.kicad_pcb"))
    except (OSError, ValueError, KeyError) as e:
        res["error"] = str(e)
    with lock:
        with open(HERE / "fan.jsonl", "a") as f:
            f.write(json.dumps(res) + "\n")
        print(json.dumps(res), flush=True)


if __name__ == "__main__":
    lock = threading.Lock()
    base = 9800 + 10 * SKIP
    ts = [threading.Thread(target=run_board, args=(b, base + i, lock)) for i, b in enumerate(BOARDS)]
    for t in ts:
        t.start()
    for t in ts:
        t.join()
