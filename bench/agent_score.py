#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Score boards that an agent routed by hand through `agents/relay.py` (doc 19 §7), against the arms of an earlier `bench/agent_eval.py` run.

  bench/agent_score.py --set srj20-agent-heldout --sessions DIR --baseline bench/records/m18/agent2-heldout.json --name model-heldout

DIR holds one folder per board (the relay's session folder: `transcript.jsonl`, `session.log`) and the board each agent saved as `<DIR>/<board>/routed.kicad_pcb`.
A board is scored the way `agent_eval.py` scores an arm: connections routed = unconnected items of the input minus those of the saved board (`tracemaker drc`
beside the input's project files), errors added relative to the input. Work is read from the transcript (the largest total the session reported); calls are
counted from it. The agent's own report is never used. The baseline file supplies the input's open connections and the plain, control and scripted-agent arms.

Writes bench/results/<name>/model.json (one row per board and a summary).
"""
import argparse
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
from agent_eval import ROOT, added, drc, read_set  # noqa: E402


def transcript_stats(path):
    work, calls, routes, kept = 0, 0, 0, 0
    for line in path.read_text().splitlines():
        e = json.loads(line)
        calls += 1
        a = e.get("answer") or {}
        work = max(work, a.get("work_used", 0), (a.get("work") or {}).get("total_used", 0))
        if e["request"]["tool"] == "route":
            routes += 1
            kept += bool(a.get("committed"))
    return {"work": work, "calls": calls, "route_calls": routes, "routes_kept": kept}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--set", required=True)
    ap.add_argument("--sessions", required=True)
    ap.add_argument("--baseline", required=True)
    ap.add_argument("--name", required=True)
    a = ap.parse_args()
    fixtures, boards = read_set(a.set)
    base = {r["board"]: r for r in json.loads(pathlib.Path(a.baseline).read_text())["rows"]}
    out = ROOT / "bench/results" / a.name
    (out / "boards").mkdir(parents=True, exist_ok=True)
    rows = []
    for b in boards:
        sess = pathlib.Path(a.sessions) / b
        saved, tr = sess / "routed.kicad_pcb", sess / "transcript.jsonl"
        r = base[b]
        row = {"board": b, "open": r["connections_open"], "plain": r["plain"]["routed"], "control": r["control"]["routed"], "scripted": r["agent"]["routed"],
               "control_work": r["control"]["work"]}
        if saved.is_file() and tr.is_file():
            board = fixtures / b / "unrouted.kicad_pcb"
            _, init_err = drc(board, out / "boards" / f"{b}.init.json")
            left, err = drc(saved, out / "boards" / f"{b}.drc.json", board)
            row.update(model=r["connections_open"] - left, added_errors=added(init_err, err), **transcript_stats(tr))
        else:
            row["model"] = None
        rows.append(row)
        print(f"{b:10} open {row['open']:>3}  plain {row['plain']:>3}  control {row['control']:>3}  scripted {row['scripted']:>3}  model {str(row['model']):>4}"
              f"  work {row.get('work', 0) / 1e6:.0f} M / {row['control_work'] / 1e6:.0f} M  route calls {row.get('route_calls', '-')}")
    ok = [r for r in rows if r["model"] is not None]
    s = {"set": a.set, "boards": len(rows), "scored": len(ok), "open": sum(r["open"] for r in ok)}
    for arm in ("plain", "control", "scripted", "model"):
        s[arm] = sum(r[arm] for r in ok)
    s.update(work=sum(r["work"] for r in ok), control_work=sum(r["control_work"] for r in ok),
             model_better_than_control=sum(r["model"] > r["control"] for r in ok), model_worse_than_control=sum(r["model"] < r["control"] for r in ok),
             model_worse_than_plain=sum(r["model"] < r["plain"] for r in ok), added_errors=sum(len(r["added_errors"]) for r in ok))
    (out / "model.json").write_text(json.dumps({"summary": s, "rows": rows}, indent=1))
    print(json.dumps(s))


if __name__ == "__main__":
    main()
