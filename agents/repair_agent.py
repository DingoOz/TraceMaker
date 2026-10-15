#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A scripted baseline agent for the board session (doc 19 §7). No model, no network: rules over what the tools report.

It spends the session's work like this: one `route` of everything with a portfolio (what `tracemaker route` does), then repair rounds. A round takes a few open
connections, removes the session copper of the nets the router named as their blockers, and routes those nets again with the stuck nets first and a fresh set
of variants; if the board then has more open connections than before the round, the round is undone. It stops when nothing is open, when the work is spent, or
after --rounds.

  --policy tabu     (version 1) the shortest connections not tried before, each connection once, the same first blockers; a round that does not reduce the open
                    connections is undone. Stops when every open connection has been tried, which leaves most of the allowance unspent.
  --policy persist  (version 2) the least-tried connections first, again and again, with a different blocker window and seed each time; a round that does not
                    make things worse is kept (a plateau move: another connection may be left open instead), so the whole allowance is spent.

It exists to answer one question with a number (does driving the engine connection by connection beat the same compute spent on more variants?) and as the
smallest example of an agent. It is deliberately simple: a model is expected to do better or the interface is not worth more work.

    repair_agent.py BOARD OUT --tracemaker TM --work-limit N [--log FILE] [--work 20000000] [--variants 8] [--threads 8] [--rounds 12]
Prints one JSON line at the end: calls, rounds kept and undone, open connections, work used.
"""
import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from session_client import Session, SessionError  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("out")
    ap.add_argument("--tracemaker", required=True)
    ap.add_argument("--work-limit", type=int, required=True)
    ap.add_argument("--log")
    ap.add_argument("--work", type=int, default=20_000_000)
    ap.add_argument("--variants", type=int, default=8)
    ap.add_argument("--repair-variants", type=int, default=4)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--rounds", type=int, default=12)
    ap.add_argument("--stuck", type=int, default=3, help="stuck connections taken per round")
    ap.add_argument("--rip", type=int, default=4, help="blocker nets removed per round")
    ap.add_argument("--policy", choices=["tabu", "persist"], default="tabu")
    a = ap.parse_args()

    kept = undone = calls = 0
    tried, count = set(), {}
    with Session(a.tracemaker, a.board, a.out, log=a.log, work_limit=a.work_limit, threads=a.threads) as s:
        def call(tool, **args):
            nonlocal calls
            calls += 1
            return s.call(tool, **args)

        # "report": the first call is the command line's route whatever the engine's DRC says of it (on some boards that DRC counts edge errors KiCad does not);
        # the repair rounds below are judged against the state they start from, so they may add nothing.
        call("route", work=a.work, variants=a.variants, verify="report")
        for rnd in range(a.rounds):
            # Size each repair so that a call cannot spend more than half of what is left: a router spends at most its budget (and a little more).
            left = call("board_info")["work_left"]
            work = min(a.work, left // (2 * a.repair_variants))
            if work < 500_000:
                break
            oc = call("open_connections", limit=512)
            if oc["open"] == 0:
                break
            before, base = oc["open"], call("history")["current"]
            if a.policy == "tabu":
                stuck = sorted(oc["connections"], key=lambda c: (c.get("length_mm", 0.0), c["net"], c["a"], c["b"]))
                skip = tried
            else:
                stuck = sorted(oc["connections"], key=lambda c: (count.get((c["net"], c["a"], c["b"]), 0), c.get("length_mm", 0.0), c["net"], c["a"], c["b"]))
                skip = set()
            pick, seen = [], set()
            for c in stuck:
                key = (c["net"], c["a"], c["b"])
                if key not in skip and c["net"] not in seen:
                    pick.append(c)
                    seen.add(c["net"])
                if len(pick) == a.stuck:
                    break
            if not pick:
                break
            tried.update((c["net"], c["a"], c["b"]) for c in pick)
            for c in pick:
                count[(c["net"], c["a"], c["b"])] = count.get((c["net"], c["a"], c["b"]), 0) + 1
            nets = [c["net"] for c in pick]
            blockers = []
            for c in pick:
                bl = c.get("blockers", [])
                if a.policy == "persist" and bl:
                    shift = (count[(c["net"], c["a"], c["b"])] - 1) * 2 % len(bl)  # a different window of the named blockers each time
                    bl = bl[shift:] + bl[:shift]
                for b in bl:
                    if b not in nets and b not in blockers:
                        blockers.append(b)
            for b in blockers[: a.rip]:
                r = s.call("rip_net", raise_errors=False, net=b)
                if "error" not in r:
                    nets.append(b)
            try:
                r = call("route", nets=nets, hints={"version": 1, "first_nets": [c["net"] for c in pick]}, work=work, variants=a.repair_variants, seed=rnd + 2)
            except SessionError as e:
                if e.code == "limit":
                    call("restore", state=base)
                    break
                raise
            after = call("open_connections", limit=1)["open"]
            if after < before or (a.policy == "persist" and after == before):
                kept += 1
            else:
                undone += 1
                call("restore", state=base)
        final = call("open_connections", limit=1)["open"]
        info = call("board_info")
        call("save")
    print(json.dumps({"calls": calls, "rounds_kept": kept, "rounds_undone": undone, "open": final, "work_used": info["work_used"]}))


if __name__ == "__main__":
    main()
