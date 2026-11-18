#!/usr/bin/env python3
"""sample152, open connection bga_conn_055: from the best board (2 open), strip the copper of 055 and the three nets that
block it (053 and 041 through its south-west via site, 043 in the corridor south) and route only those four, everything
else fixed. Arm A pre-places 055's dog-bone (F.Cu stub + via at (0.4, -2.8)), arm B does not. usage: fix.py ARM..."""
import json, os, pathlib, subprocess, sys, threading, time
HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent.parent / "srj20-never")); sys.argv, ARGS = sys.argv[:1], sys.argv[1:]
import drive  # noqa: E402
LIMIT, SEED = os.environ.get("LIMIT", "900"), os.environ.get("SEED", "403")
NETS = os.environ["NETS"]


def common():
    c = list(drive.COMMON)
    c[c.index("--threads") + 1] = os.environ.get("THREADS", "8")
    c[c.index("--time") + 1] = os.environ.get("MAIN", LIMIT)
    c[c.index("--time-total") + 1] = LIMIT
    return c


def run_arm(arm, port, lock):
    d = HERE / arm
    (d / "out.kicad_pro").write_bytes((d / "board.kicad_pro").read_bytes())
    cmd = [str(HERE / "tracemaker"), "route", str(d / "board.kicad_pcb"), "-o", str(d / "out.kicad_pcb"), "--json", str(d / "out.json"),
           *common(), *(["--only-net", NETS] if NETS else os.environ["EXTRA"].split()), "--seed", SEED, "--view", "--view-port", str(port)]
    (d / "cmd.txt").write_text(" ".join(cmd) + "\n")
    t0 = time.time()
    subprocess.run(cmd, stdout=open(d / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True)
    res = {"arm": arm, "seed": SEED, "seconds": round(time.time() - t0)}
    try:
        j = json.load(open(d / "out.json"))
        res.update({"open_json": j["connections"] - j["routed"], "vias": j.get("vias")})
        res.update(drive.judge("sample152", d / "out.kicad_pcb"))
    except (OSError, ValueError, KeyError) as e:
        res["error"] = str(e)
    with lock:
        with open(HERE / "fix.jsonl", "a") as f:
            f.write(json.dumps(res) + "\n")
        print(json.dumps(res), flush=True)


if __name__ == "__main__":
    lock = threading.Lock()
    ts = [threading.Thread(target=run_arm, args=(a, int(os.environ.get("PORT", "9900")) + i, lock)) for i, a in enumerate(ARGS)]
    for t in ts: t.start()
    for t in ts: t.join()
