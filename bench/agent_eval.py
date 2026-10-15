#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Does an agent that drives the board session route more than the plain route at the same compute? (doc 19 §7, M18)

Per board, three arms, each scored the same way: connections routed = unconnected items of the input board minus unconnected items of the written board
(`tracemaker drc`), and the errors the run added (any added error invalidates the arm for that board).

  plain    route --work W --variants V                    (what `tracemaker route` does)
  control  route --work W --variants V+4                  (more compute without an agent: the equal-compute reference; its work is the agent's limit)
  agent    AGENT BOARD OUT --work-limit <control's work>  (a program that drives `tracemaker session`, e.g. agents/repair_agent.py)

Compute = search expansions plus flood cells over all routers (route.json `work_all_variants.total`; the session's `work_used`). The agent's own thinking is
not routing work. `--time 3600` on purpose: a wall limit would make the work budget not the only stop (doc 17 §8.1).

  bench/agent_eval.py --set srj20-m15fail4 --half odd --name agent-odd --agent "python3 agents/repair_agent.py" [--work 20000000] [--variants 8]

Writes bench/results/<name>/agent.json (one row per board, and a summary).
"""
import argparse
import concurrent.futures as cf
import json
import os
import pathlib
import shlex
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))


def read_set(name):
    fixtures, boards = None, []
    for line in (ROOT / "bench/sets" / f"{name}.txt").read_text().splitlines():
        if line.startswith("#fixtures:"):
            fixtures = ROOT / line.split(":", 1)[1].strip()
        elif line.strip() and not line.startswith("#"):
            boards.append(line.strip())
    return fixtures, boards


def drc(board, tmp, rules_from=None):
    """(unconnected items, error violations by type) of a board, from the engine's DRC. A board is judged by the project files beside it, so a written board is
    copied beside copies of the input's project and rules first (the same reason the skills' sign-off judges both boards in one place)."""
    js = pathlib.Path(tmp)
    if rules_from is not None:
        stage = pathlib.Path(board).parent / (pathlib.Path(board).stem + ".judge")
        stage.mkdir(exist_ok=True)
        for ext in (".kicad_pro", ".kicad_dru"):
            if rules_from.with_suffix(ext).is_file():
                shutil.copyfile(rules_from.with_suffix(ext), stage / ("judged" + ext))
        shutil.copyfile(board, stage / "judged.kicad_pcb")
        board = stage / "judged.kicad_pcb"
    subprocess.run([str(TM), "drc", str(board), "--json", str(js)], capture_output=True, text=True, timeout=600)
    d = json.loads(js.read_text())
    errors = {}
    for v in d["violations"]:
        if v.get("severity") == "error":
            errors[v["type"]] = errors.get(v["type"], 0) + 1
    return len(d["unconnected_items"]), errors


def added(before, after):
    return {k: n - before.get(k, 0) for k, n in after.items() if n > before.get(k, 0)}


def work_of(route_json):
    t = route_json["work_all_variants"]["total"]
    return t["expansions"] + t["flood_cells"]


def run_board(job):
    fixtures, name, out, a = job
    d = out / "boards"
    d.mkdir(parents=True, exist_ok=True)
    board = fixtures / name / "unrouted.kicad_pcb"
    row = {"board": name}
    init_open, init_err = drc(board, d / f"{name}.init.json")
    row["connections_open"] = init_open
    for arm, variants in (("plain", a.variants), ("control", a.variants + a.extra_variants)):
        out_pcb, js = d / f"{name}.{arm}.kicad_pcb", d / f"{name}.{arm}.json"
        t0 = time.time()
        subprocess.run([str(TM), "route", str(board), "-o", str(out_pcb), "--work", str(a.work), "--variants", str(variants), "--threads", str(a.threads),
                        "--no-kb", "--time", "3600", "--json", str(js)], capture_output=True, text=True)
        if not js.exists() or not out_pcb.exists():
            row[arm] = {"error": "no result"}
            continue
        left, err = drc(out_pcb, d / f"{name}.{arm}.drc.json", board)
        row[arm] = {"routed": init_open - left, "work": work_of(json.loads(js.read_text())), "added_errors": added(init_err, err), "seconds": round(time.time() - t0, 1)}
    if "work" not in row.get("control", {}):
        row["agent"] = {"error": "no control, so no work limit"}
        return row
    out_pcb, log = d / f"{name}.agent.kicad_pcb", d / f"{name}.agent.log"
    t0 = time.time()
    p = subprocess.run(shlex.split(a.agent) + [str(board), str(out_pcb), "--tracemaker", str(TM), "--work-limit", str(row["control"]["work"]), "--log", str(log),
                                               "--work", str(a.work), "--variants", str(a.variants), "--threads", str(a.threads)],
                       capture_output=True, text=True, timeout=a.agent_timeout)
    if p.returncode != 0 or not out_pcb.exists():
        row["agent"] = {"error": (p.stderr or p.stdout)[-300:]}
        return row
    left, err = drc(out_pcb, d / f"{name}.agent.drc.json", board)
    summary = json.loads(p.stdout.strip().splitlines()[-1])
    row["agent"] = {"routed": init_open - left, "work": summary["work_used"], "added_errors": added(init_err, err), "seconds": round(time.time() - t0, 1),
                    "calls": summary["calls"], "rounds_kept": summary["rounds_kept"], "rounds_undone": summary["rounds_undone"]}
    return row


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", required=True)
    ap.add_argument("--name", required=True)
    ap.add_argument("--agent", required=True, help="command that takes BOARD OUT --tracemaker TM --work-limit N --log FILE --work W --variants V --threads T")
    ap.add_argument("--work", type=int, default=20_000_000)
    ap.add_argument("--variants", type=int, default=8)
    ap.add_argument("--extra-variants", type=int, default=4, help="the control's extra variants (its work is the agent's limit)")
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--agent-timeout", type=int, default=3600)
    ap.add_argument("--half", choices=["even", "odd", "all"], default="all")
    ap.add_argument("--limit", type=int, default=0)
    a = ap.parse_args()
    fixtures, boards = read_set(a.set)
    boards = [b for i, b in enumerate(boards) if a.half == "all" or (i % 2 == 0) == (a.half == "even")]
    if a.limit:
        boards = boards[: a.limit]
    out = ROOT / "bench/results" / a.name
    out.mkdir(parents=True, exist_ok=True)
    rows = []
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for row in ex.map(run_board, [(fixtures, b, out, a) for b in boards]):
            rows.append(row)
            cell = lambda arm: str(row.get(arm, {}).get("routed", "-"))
            print(f"{row['board']:12} open {row.get('connections_open', '-'):>3}  plain {cell('plain'):>3}  control {cell('control'):>3}  agent {cell('agent'):>3}"
                  f"  work agent/control {row.get('agent', {}).get('work', '-')}/{row.get('control', {}).get('work', '-')}", flush=True)
    ok = [r for r in rows if all("routed" in r.get(arm, {}) for arm in ("plain", "control", "agent"))]
    s = {"set": a.set, "half": a.half, "boards": len(rows), "scored": len(ok), "work": a.work, "variants": a.variants, "agent": a.agent,
         "open": sum(r["connections_open"] for r in ok),
         **{f"{arm}_routed": sum(r[arm]["routed"] for r in ok) for arm in ("plain", "control", "agent")},
         "agent_work": sum(r["agent"]["work"] for r in ok), "control_work": sum(r["control"]["work"] for r in ok), "plain_work": sum(r["plain"]["work"] for r in ok),
         "agent_better_than_plain": sum(1 for r in ok if r["agent"]["routed"] > r["plain"]["routed"]),
         "agent_better_than_control": sum(1 for r in ok if r["agent"]["routed"] > r["control"]["routed"]),
         "agent_worse_than_control": sum(1 for r in ok if r["agent"]["routed"] < r["control"]["routed"]),
         "added_errors": sum(len(r[arm]["added_errors"]) for r in ok for arm in ("plain", "control", "agent"))}
    (out / "agent.json").write_text(json.dumps({"summary": s, "rows": rows}, indent=1))
    print(json.dumps(s))
    return 0


if __name__ == "__main__":
    sys.exit(main())
