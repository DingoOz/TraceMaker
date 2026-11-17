#!/usr/bin/env python3
"""srj20 boards still open: repeat the clearance ramp with new seeds from the best boards found so far, two boards at a time.

Each round takes, of the four open boards closest to complete, the two that have had the fewest rounds, and runs six arms on each
(4 threads per arm, 1 h): arms 1-3 warm from the best board so far, arms 4-5 from the second best, arm 6 from the third (distinct
boards from srj20-never, srj20-loose, earlier rounds and the October best), each arm its own new seed. The first arm to route every
connection stops its board's other arms; KiCad DRC judges it. Rounds stop starting after DEADLINE seconds. One line per board-round
in rounds.jsonl.
"""
import json, os, pathlib, signal, subprocess, sys, threading, time

HERE = pathlib.Path(__file__).resolve().parent
RES = HERE.parent
sys.path.insert(0, str(RES / "srj20-never"))
sys.argv = sys.argv[:1]
import drive  # noqa: E402

TM = RES / "srj20-never" / "tracemaker"
DEADLINE = 7 * 3600
COMMON = list(drive.COMMON)
COMMON[COMMON.index("--threads") + 1] = "4"
CLEAN = {"sample128", "sample103", "sample072", "sample096"}


def pool(b):
    """Every saved routing of b at the real rules: (open, path, what)."""
    info = drive.BOARDS[b]
    out = [(info["best_open"], info["best_pcb"], f"October best ({info['best_run']})")]
    for run in ("srj20-never", "srj20-loose", "srj20-again"):
        for j in sorted((RES / run).glob(f"{b}/**/out.json")):
            try:
                r = json.load(open(j))
            except (OSError, ValueError):
                continue
            pcb = j.parent / "out.kicad_pcb"
            if "loose" in j.parts or not pcb.exists() or "routed" not in r:
                continue
            out.append((r["connections"] - r["routed"], str(pcb), str(j.parent.relative_to(RES))))
    return sorted(out, key=lambda x: (x[0], x[2]))


RECIPES = {  # arm -> extra options; every arm warm from the best board so far
    "a1": ["--lns-open", "8"],
    "a2": ["--lns-open", "8", "--lns-open-radius-mm", "5"],
    "a3": ["--island-slack", "2"],
    "a4": ["--island-slack", "2", "--lns-open", "8"],
    "a5": ["--ramp-from", "0.04"],
    "a6": ["--ramp-fresh", "1"],
}
REPEAT = os.environ.get("AGAIN_REPEAT") == "1"  # round 1 recipe: arms 1-3 best, 4-5 second, 6 third, no extra options


def run_board(b, rnd, port0, lock):
    starts = pool(b)
    if REPEAT:
        picks = [starts[0]] * 3 + [starts[min(1, len(starts) - 1)]] * 2 + [starts[min(2, len(starts) - 1)]]
    else:
        picks = [starts[0]] * 6
    d = HERE / b / f"r{rnd}"
    d.mkdir(parents=True, exist_ok=True)
    procs, t0 = {}, time.time()
    for k, (op, warm, what) in enumerate(picks):
        name, seed = f"a{k + 1}", 100 * rnd + 20 + k + 1
        a = d / name
        a.mkdir(exist_ok=True)
        (a / "out.kicad_pro").write_bytes((drive.FIX / b / "unrouted.kicad_pro").read_bytes())
        extra = [] if REPEAT else RECIPES[name]
        common = list(COMMON)
        if "--ramp-from" in extra:
            del common[common.index("--ramp-from"):common.index("--ramp-from") + 2]
        cmd = [str(TM), "route", str(drive.FIX / b / "unrouted.kicad_pcb"), "-o", str(a / "out.kicad_pcb"), "--json",
               str(a / "out.json"), *common, *extra, "--seed", str(seed), "--warm-pcb", warm, "--view", "--view-port", str(port0 + k)]
        (a / "cmd.txt").write_text(" ".join(cmd) + f"\n# warm start: {what}, {op} open\n")
        procs[name] = (subprocess.Popen(cmd, stdout=open(a / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True),
                       seed, warm, what, op)
    allp = dict(procs)
    winner = None
    while procs and not winner:
        time.sleep(10)
        for name, (p, *rest) in list(procs.items()):
            if p.poll() is None:
                continue
            del procs[name]
            try:
                j = json.load(open(d / name / "out.json"))
            except (OSError, ValueError):
                continue
            if j.get("routed") == j.get("connections"):
                winner = name
                break
    for p, *_ in procs.values():  # the arms this script started, by their own handles
        os.killpg(p.pid, signal.SIGTERM)
    for p, *_ in procs.values():
        p.wait()
    res = {"board": b, "round": rnd, "best_open_before": starts[0][0], "seconds": round(time.time() - t0), "arms": {}}
    for name, (p, seed, warm, what, op) in allp.items():
        try:
            j = json.load(open(d / name / "out.json"))
            res["arms"][name] = {"seed": seed, "start": what, "start_open": op, "open": j["connections"] - j["routed"]}
        except (OSError, ValueError, KeyError):
            res["arms"][name] = {"seed": seed, "start": what, "start_open": op, "open": None}
        res["arms"][name]["options"] = [] if REPEAT else RECIPES[name]
    done = {n: a for n, a in res["arms"].items() if a["open"] is not None}
    if done:
        name = winner or min(done, key=lambda n: (done[n]["open"], n))
        res.update({"arm": name, "open": done[name]["open"], "completed": bool(winner)})
        res.update(drive.judge(b, d / name / "out.kicad_pcb"))
    with lock:
        with open(HERE / "rounds.jsonl", "a") as f:
            f.write(json.dumps(res) + "\n")
        print(json.dumps(res), flush=True)


if __name__ == "__main__":
    start, lock = time.time(), threading.Lock()
    rounds = {}
    if (HERE / "rounds.jsonl").exists():
        for r in map(json.loads, open(HERE / "rounds.jsonl")):
            rounds[r["board"]] = max(rounds.get(r["board"], 0), r["round"])
            if r.get("clean"):
                CLEAN.add(r["board"])
    while time.time() - start < DEADLINE:
        open_b = [b for b in drive.BOARDS if b not in CLEAN]
        best = {b: pool(b)[0][0] for b in open_b}
        near = sorted(open_b, key=lambda b: (best[b], b))[:4]  # the four closest boards, in turn
        pick = sorted(near, key=lambda b: (rounds.get(b, 0), best[b], b))[:2]
        ts = []
        for slot, b in enumerate(pick):
            rounds[b] = rounds.get(b, 0) + 1
            ts.append(threading.Thread(target=run_board, args=(b, rounds[b], 8901 + 20 * slot, lock)))
        for t in ts:
            t.start()
        for t in ts:
            t.join()
        for r in map(json.loads, open(HERE / "rounds.jsonl")):
            if r.get("clean"):
                CLEAN.add(r["board"])
    print("deadline", flush=True)
