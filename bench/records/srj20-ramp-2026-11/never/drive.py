#!/usr/bin/env python3
"""srj20 boards never clean: one board at a time, six arms of the clearance ramp warm-started from earlier results.

Each arm: relax portfolio, clearance ramp from 0.06 mm, warm start from a saved board, its own seed, 8 threads, 1 h.
The first arm to route every connection stops the others; KiCad DRC judges it. One JSON line per board in results.jsonl.
"""
import json, os, pathlib, signal, subprocess, sys, time

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[2]
sys.path.insert(0, str(ROOT / "bench"))
import run, drc_judge  # noqa: E402

FIX = pathlib.Path("/home/dingo/Programming/TraceMaker/bench/data/tscircuit/boards/srj20")
TM = HERE / "tracemaker"
BOARDS = json.load(open(ROOT / "bench/results/srj20-never15.json"))
TODAY = ROOT / "bench/results/srj20-open17/boards"
LIMIT = 3600
COMMON = ["--relax", "--variants", "16", "--threads", "8", "--fast-bends", "--no-kb", "--round-work", "20000000", "--rounds", "1000",
          "--time", str(LIMIT), "--time-total", str(LIMIT),
          "--ramp-from", "0.06", "--ramp-step", "0.02", "--ramp-rounds", "3", "--ramp-cycle", "2", "--ramp-stall", "2"]


def arms(b, info):
    second = info["loose_pcb"] or str(TODAY / f"{b}.kicad_pcb")
    second_what = "complete routing at 0.06 mm clearance (diag-c006)" if info["loose_pcb"] else "srj20-open17 (latest code, 1 h)"
    best_what = f"best earlier result, {info['best_open']} open ({info['best_run']})"
    return [(f"a{i}", 10 + i, info["best_pcb"], best_what) for i in range(1, 5)] + \
           [(f"a{i}", 10 + i, second, second_what) for i in range(5, 7)]


def judge(b, out):
    before, after = run.drc(FIX / b / "unrouted.kicad_pcb"), run.drc(out)
    if before is None or after is None:
        return {"judge": "failed"}
    r = drc_judge.added_errors(before, after)
    return {"unconnected_after": after["unconnected"], "added_errors": r["added_errors"], "error_gate": r["error_gate"],
            "clean": drc_judge.clean(r, after["unconnected"])}


def board(b, info, port0):
    d = HERE / b
    d.mkdir(exist_ok=True)
    procs = {}
    t0 = time.time()
    for k, (name, seed, warm, what) in enumerate(arms(b, info)):
        a = d / name
        a.mkdir(exist_ok=True)
        (a / "out.kicad_pro").write_bytes((FIX / b / "unrouted.kicad_pro").read_bytes())
        cmd = [str(TM), "route", str(FIX / b / "unrouted.kicad_pcb"), "-o", str(a / "out.kicad_pcb"), "--json", str(a / "out.json"),
               *COMMON, "--seed", str(seed), "--warm-pcb", warm, "--view", "--view-port", str(port0 + k)]
        (a / "cmd.txt").write_text(" ".join(cmd) + f"\n# warm start: {what}\n")
        procs[name] = (subprocess.Popen(cmd, stdout=open(a / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True), seed, warm, what)
    winner = None
    while procs and not winner:
        time.sleep(10)
        for name, (p, seed, warm, what) in list(procs.items()):
            if p.poll() is None:
                continue
            del procs[name]
            try:
                j = json.load(open(d / name / "out.json"))
            except (OSError, ValueError):
                continue
            if j.get("routed") == j.get("connections"):
                winner = (name, seed, warm, what, j)
                break
    for p, *_ in procs.values():  # the arms this driver started, by their own handles
        os.killpg(p.pid, signal.SIGTERM)
    for p, *_ in procs.values():
        p.wait()
    res = {"board": b, "connections": info["connections"], "previous_best_open": info["best_open"], "previous_best_run": info["best_run"],
           "seconds": round(time.time() - t0)}
    if winner:
        name, seed, warm, what, j = winner
        res.update({"routed": j["routed"], "arm": name, "seed": seed, "warm_start": what, "warm_pcb": warm, "route_seconds": round(j["seconds"])})
        res.update(judge(b, d / name / "out.kicad_pcb"))
    else:
        best = None
        for name, *_ in arms(b, info):
            try:
                j = json.load(open(d / name / "out.json"))
            except (OSError, ValueError):
                continue
            if best is None or j["routed"] > best[1]["routed"]:
                best = (name, j)
        if best:
            res.update({"routed": best[1]["routed"], "arm": best[0]})
            res.update(judge(b, d / best[0] / "out.kicad_pcb"))
    with open(HERE / "results.jsonl", "a") as f:
        f.write(json.dumps(res) + "\n")
    print(json.dumps(res), flush=True)


if __name__ == "__main__":
    done = set()
    if (HERE / "results.jsonl").exists():
        done = {json.loads(l)["board"] for l in open(HERE / "results.jsonl")}
    for b, info in BOARDS.items():
        if b not in done:
            board(b, info, 8901)
