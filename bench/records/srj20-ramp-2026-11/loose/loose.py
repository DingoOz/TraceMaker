#!/usr/bin/env python3
"""srj20 boards still open after srj20-never: make a start that is complete at 0.06 mm clearance, then ramp it to the real rules.

Waits for srj20-never/results.jsonl to hold all 15 boards. Stage 1 (three boards at a time, 16 threads, 30 min): route a copy of
the board whose project file says 0.06 mm clearance (the diag-c006 recipe that gave 103 and 072 their starts). Stage 2 (one board
at a time, six arms x 8 threads, 1 h): the srj20-never ramp, arms a1-a4 warm from the loose board, a5-a6 from the best board so
far; only for boards whose loose start has fewer open connections than that best. Results in loose.jsonl and results.jsonl.
"""
import json, os, pathlib, signal, subprocess, sys, time

HERE = pathlib.Path(__file__).resolve().parent
NEVER = HERE.parent / "srj20-never"
sys.path.insert(0, str(NEVER))
sys.argv = sys.argv[:1]
import drive  # noqa: E402  (judge, COMMON, FIX, BOARDS; its main loop only runs as __main__)

TM = NEVER / "tracemaker"
LOOSE = ["--variants", "16", "--threads", "16", "--no-kb", "--round-work", "60000000", "--rounds", "200",
         "--time", "1800", "--time-total", "1800"]


def wait_for_never():
    while True:
        p = NEVER / "results.jsonl"
        if p.exists() and len(p.read_text().splitlines()) >= len(drive.BOARDS):
            return {j["board"]: j for j in map(json.loads, p.read_text().splitlines())}
        time.sleep(30)


def best_so_far(b, info, res):
    open_now = res["connections"] - res.get("routed", 0)
    if open_now < info["best_open"]:
        return open_now, str(NEVER / b / res["arm"] / "out.kicad_pcb"), f"srj20-never {res['arm']}, {open_now} open"
    return info["best_open"], info["best_pcb"], f"best earlier result, {info['best_open']} open ({info['best_run']})"


def loose_copy(b):
    d = HERE / "loose" / b
    d.mkdir(parents=True, exist_ok=True)
    pro = json.load(open(drive.FIX / b / "unrouted.kicad_pro"))
    pro["board"]["design_settings"]["rules"]["min_clearance"] = 0.06
    for c in pro["net_settings"]["classes"]:
        c["clearance"] = min(c["clearance"], 0.06)
    json.dump(pro, open(d / "in.kicad_pro", "w"), indent=2)
    (d / "in.kicad_pcb").write_bytes((drive.FIX / b / "unrouted.kicad_pcb").read_bytes())
    json.dump(pro, open(d / "out.kicad_pro", "w"), indent=2)
    return d


def stage1(todo):
    done = {}
    if (HERE / "loose.jsonl").exists():
        done = {j["board"]: j for j in map(json.loads, open(HERE / "loose.jsonl"))}
    queue = [b for b in todo if b not in done]
    running = {}
    while queue or running:
        while queue and len(running) < 3:
            b = queue.pop(0)
            d = loose_copy(b)
            port = next(p for p in range(8911, 8920) if p not in {r[2] for r in running.values()})
            cmd = [str(TM), "route", str(d / "in.kicad_pcb"), "-o", str(d / "out.kicad_pcb"), "--json", str(d / "out.json"),
                   *LOOSE, "--view", "--view-port", str(port)]
            (d / "cmd.txt").write_text(" ".join(cmd) + "\n")
            running[b] = (subprocess.Popen(cmd, stdout=open(d / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True),
                          time.time(), port)
        time.sleep(15)
        for b, (p, t0, _) in list(running.items()):
            if p.poll() is None:
                continue
            del running[b]
            try:
                j = json.load(open(HERE / "loose" / b / "out.json"))
                r = {"board": b, "connections": j["connections"], "routed": j["routed"], "seconds": round(time.time() - t0),
                     "pcb": str(HERE / "loose" / b / "out.kicad_pcb")}
            except (OSError, ValueError, KeyError):
                r = {"board": b, "failed": True, "seconds": round(time.time() - t0)}
            done[b] = r
            with open(HERE / "loose.jsonl", "a") as f:
                f.write(json.dumps(r) + "\n")
            print("loose", json.dumps(r), flush=True)
    return done


def stage2(b, info, loose, best, port0=8901):
    best_open, best_pcb, best_what = best
    d = HERE / b
    d.mkdir(exist_ok=True)
    lo = loose["connections"] - loose["routed"]
    arms = [(f"a{i}", 10 + i, loose["pcb"], f"0.06 mm routing, {lo} open (srj20-loose stage 1)") for i in range(1, 5)] + \
           [(f"a{i}", 10 + i, best_pcb, best_what) for i in range(5, 7)]
    procs, t0 = {}, time.time()
    for k, (name, seed, warm, what) in enumerate(arms):
        a = d / name
        a.mkdir(exist_ok=True)
        (a / "out.kicad_pro").write_bytes((drive.FIX / b / "unrouted.kicad_pro").read_bytes())
        cmd = [str(TM), "route", str(drive.FIX / b / "unrouted.kicad_pcb"), "-o", str(a / "out.kicad_pcb"), "--json",
               str(a / "out.json"), *drive.COMMON, "--seed", str(seed), "--warm-pcb", warm, "--view", "--view-port", str(port0 + k)]
        (a / "cmd.txt").write_text(" ".join(cmd) + f"\n# warm start: {what}\n")
        procs[name] = (subprocess.Popen(cmd, stdout=open(a / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True),
                       seed, warm, what)
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
    for p, *_ in procs.values():  # the arms this script started, by their own handles
        os.killpg(p.pid, signal.SIGTERM)
    for p, *_ in procs.values():
        p.wait()
    res = {"board": b, "connections": info["connections"], "best_open_before": best_open, "best_before": best_what,
           "loose_open": lo, "loose_seconds": loose["seconds"], "seconds": round(time.time() - t0), "arms": {}}
    for name, *_ in arms:
        try:
            j = json.load(open(d / name / "out.json"))
            res["arms"][name] = j["connections"] - j["routed"]
        except (OSError, ValueError, KeyError):
            pass
    if winner:
        name, seed, warm, what, j = winner
        res.update({"routed": j["routed"], "arm": name, "seed": seed, "warm_start": what, "warm_pcb": warm,
                    "route_seconds": round(j["seconds"])})
    elif res["arms"]:
        name = min(res["arms"], key=lambda n: (res["arms"][n], n))
        res.update({"routed": info["connections"] - res["arms"][name], "arm": name})
    if "arm" in res:
        res.update(drive.judge(b, d / res["arm"] / "out.kicad_pcb"))
    with open(HERE / "results.jsonl", "a") as f:
        f.write(json.dumps(res) + "\n")
    print("ramp", json.dumps(res), flush=True)


if __name__ == "__main__":
    never = wait_for_never()
    print("srj20-never finished", flush=True)
    best = {b: best_so_far(b, info, never[b]) for b, info in drive.BOARDS.items() if not never[b].get("clean")}
    todo = sorted(best, key=lambda b: best[b][0])
    loose = stage1(todo)
    done = set()
    if (HERE / "results.jsonl").exists():
        done = {json.loads(l)["board"] for l in open(HERE / "results.jsonl")}
    for b in sorted(todo, key=lambda b: (loose[b].get("connections", 0) - loose[b].get("routed", 0), best[b][0])):
        if b in done or loose[b].get("failed"):
            continue
        if loose[b]["connections"] - loose[b]["routed"] >= best[b][0]:
            print("skip", b, "loose start is no better than", best[b][2], flush=True)
            continue
        stage2(b, drive.BOARDS[b], loose[b], best[b])
