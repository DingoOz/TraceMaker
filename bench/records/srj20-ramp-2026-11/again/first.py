#!/usr/bin/env python3
"""srj20: route first the connections that stay open in most saved results (--first-nets), from the best board and from nothing.

The saved results of a board leave the same few connections open (sample040's bga_conn_080 in all 18). For each pair of boards in
PAIRS, six arms per board (4 threads, 1 h): a1-a2 warm from the best board with those nets first, a3-a4 from nothing with those
nets first, a5 from nothing without them (the control), a6 from nothing with them first in the order listed. One line per
board in first.jsonl.
"""
import collections, glob, json, os, pathlib, signal, subprocess, sys, threading, time

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))
sys.argv = sys.argv[:1]
import again  # noqa: E402

drive = again.drive
PAIRS = [("sample040", "sample120"), ("sample152", "sample070")]


def hard_nets(b):
    c, n = collections.Counter(), 0
    for f in glob.glob(str(again.RES / f"srj20-*/{b}/**/out.json"), recursive=True):
        if "/loose/" in f:
            continue
        j = json.load(open(f))
        n += 1
        c.update({x.split(":")[0] for x in j["failures"]})
    return [k for k, v in c.most_common() if v * 2 >= n]


def arms(b):
    best = again.pool(b)[0]
    nets = ",".join(hard_nets(b))
    first = ["--first-nets", nets]
    return nets, [("a1", best, first), ("a2", best, first), ("a3", None, first), ("a4", None, first), ("a5", None, []),
                  ("a6", None, first + ["--first-nets-ordered"])]


def run_board(b, port0, lock, before):
    nets, plan = arms(b)
    d = HERE / b / "first"
    d.mkdir(parents=True, exist_ok=True)
    procs, t0 = {}, time.time()
    for k, (name, start, extra) in enumerate(plan):
        seed = 300 + k + 1
        a = d / name
        a.mkdir(exist_ok=True)
        (a / "out.kicad_pro").write_bytes((drive.FIX / b / "unrouted.kicad_pro").read_bytes())
        warm = ["--warm-pcb", start[1]] if start else []
        cmd = [str(again.TM), "route", str(drive.FIX / b / "unrouted.kicad_pcb"), "-o", str(a / "out.kicad_pcb"), "--json",
               str(a / "out.json"), *again.COMMON, *extra, "--seed", str(seed), *warm, "--view", "--view-port", str(port0 + k)]
        (a / "cmd.txt").write_text(" ".join(cmd) + f"\n# start: {start[2] + ', ' + str(start[0]) + ' open' if start else 'nothing'}\n")
        procs[name] = (subprocess.Popen(cmd, stdout=open(a / "log", "w"), stderr=subprocess.STDOUT, start_new_session=True),
                       seed, start, extra)
    allp, winner = dict(procs), None
    while procs and not winner:
        time.sleep(10)
        for name, (p, *_) in list(procs.items()):
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
    res = {"board": b, "first_nets": nets, "best_open_before": before, "seconds": round(time.time() - t0),
           "arms": {}}
    for name, (p, seed, start, extra) in allp.items():
        try:
            j = json.load(open(d / name / "out.json"))
            o = j["connections"] - j["routed"]
            still = sorted({x.split(":")[0] for x in j["failures"]} & set(nets.split(",")))
        except (OSError, ValueError, KeyError):
            o, still = None, None
        res["arms"][name] = {"seed": seed, "start": f"{start[2]}, {start[0]} open" if start else "nothing",
                             "options": extra, "open": o, "first_nets_still_open": still}
    done = {n: a for n, a in res["arms"].items() if a["open"] is not None}
    if done:
        name = winner or min(done, key=lambda n: (done[n]["open"], n))
        res.update({"arm": name, "open": done[name]["open"], "completed": bool(winner)})
        res.update(drive.judge(b, d / name / "out.kicad_pcb"))
    with lock:
        with open(HERE / "first.jsonl", "a") as f:
            f.write(json.dumps(res) + "\n")
        print(json.dumps(res), flush=True)


if __name__ == "__main__":
    lock = threading.Lock()
    for pair in PAIRS:
        before = {b: again.pool(b)[0][0] for b in pair}
        print("pair", pair, "best before", before, {b: hard_nets(b) for b in pair}, flush=True)
        ts = [threading.Thread(target=run_board, args=(b, 8901 + 20 * i, lock, before[b])) for i, b in enumerate(pair)]
        for t in ts:
            t.start()
        for t in ts:
            t.join()
