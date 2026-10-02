#!/usr/bin/env python3
"""Placement evaluation on PCBench boards (human placement vs tracemaker-place full/refine).

For each board: place in full and refine mode, re-read every output with `tracemaker inspect`, run KiCad's DRC
(kicad-cli, Docker) on the human and placed boards and diff the courtyard / edge violation counts, compare HPWL,
optionally route all three with `tracemaker route` and check the KiCad round trip (kicad_truth vs inspect --json).

Usage: src/place/eval_place.py [--route] [--truth] [--jobs N] [--out DIR] board_name ...
Output: DIR/<board>/... and DIR/summary.json, DIR/summary.md (markdown table).
"""
import argparse
import concurrent.futures as cf
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
PCBENCH = os.path.join(ROOT, "bench/data/freerouting/scripts/benchmark/fixtures/PCBench")
# DRC types that placement can introduce (courtyards, holes in courtyards, board edge).
PLACEMENT_TYPES = ["courtyards_overlap", "pth_inside_courtyard", "npth_inside_courtyard", "copper_edge_clearance",
                   "malformed_courtyard", "missing_courtyard"]


def run(cmd, cwd=None, timeout=None):
    r = subprocess.run(cmd, cwd=cwd, capture_output=True, text=True, timeout=timeout)
    return r.returncode, r.stdout + r.stderr


def drc_counts(board, out_json):
    rc, log = run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", os.path.basename(out_json),
                   os.path.basename(board)], cwd=os.path.dirname(board), timeout=900)
    if not os.path.exists(out_json):
        return None, log
    j = json.load(open(out_json))
    counts = {}
    for v in j.get("violations", []):
        counts[v["type"]] = counts.get(v["type"], 0) + 1
    return counts, ""


def place(args, board, out, mode):
    js = out.replace(".kicad_pcb", ".json")
    rc, log = run([args.place, board, "-o", out, "--mode", mode, "--json", js, "--seed", "1", "--threads", str(args.place_threads)],
                  timeout=3600)
    open(out.replace(".kicad_pcb", ".log"), "w").write(log)
    return rc, (json.load(open(js)) if os.path.exists(js) else None)


def inspect_ok(args, board):
    rc, log = run([args.tm, "inspect", board], timeout=300)
    return rc == 0


def route(args, board):
    out = board.replace(".kicad_pcb", ".routed.kicad_pcb")
    js = board.replace(".kicad_pcb", ".route.json")
    if not (args.reuse and os.path.exists(js)):
        run([args.tm, "route", board, "-o", out, "--time", str(args.route_time), "--threads", "4", "--json", js], timeout=args.route_time * 4 + 600)
    if not os.path.exists(js):
        return None
    j = json.load(open(js))
    return {"routed": j.get("routed"), "connections": j.get("connections"), "vias": j.get("vias")}


def truth(args, board):
    kj = board.replace(".kicad_pcb", ".kicad_truth.json")
    oj = board.replace(".kicad_pcb", ".ours_truth.json")
    run([os.path.join(ROOT, "scripts/kicad-python"), os.path.join(ROOT, "scripts/kicad_truth.py"), board, kj], cwd=ROOT, timeout=900)
    run([args.tm, "inspect", board, "--json", oj], timeout=300)
    rc, log = run([sys.executable, os.path.join(ROOT, "scripts/compare_truth.py"), kj, oj], timeout=300)
    return "MATCH" if rc == 0 and "MATCH" in log else ("DIFF: " + log.strip()[-300:])


def one(args, name):
    src = os.path.join(PCBENCH, name, "unrouted.kicad_pcb")
    d = os.path.join(args.out, name.replace(" ", "_"))
    os.makedirs(d, exist_ok=True)
    human = os.path.join(d, "human.kicad_pcb")
    shutil.copyfile(src, human)
    res = {"board": name}
    boards = {"human": human}
    for mode in ("full", "refine"):
        out = os.path.join(d, f"{mode}.kicad_pcb")
        if args.reuse and os.path.exists(out.replace(".kicad_pcb", ".json")):
            rc, rep = 0, json.load(open(out.replace(".kicad_pcb", ".json")))
        else:
            rc, rep = place(args, human, out, mode)
        res[mode] = {"rc": rc, "report": rep}
        boards[mode] = out
    for k, b in boards.items():
        res.setdefault(k, {})
        res[k]["inspect_ok"] = inspect_ok(args, b)
        if args.drc:
            c, err = drc_counts(b, b.replace(".kicad_pcb", ".drc.json"))
            res[k]["drc"] = c
            if err:
                res[k]["drc_error"] = err[-500:]
    if args.route:
        for k, b in boards.items():
            res[k]["route"] = route(args, b)
    if args.truth:
        for k in ("full", "refine"):
            res[k]["truth"] = truth(args, boards[k])
    return res


def fmt_table(results):
    lines = ["| Board | movable | HPWL human (mm) | HPWL full | HPWL refine | LB any-rot | crossings h/f/r | new DRC full | new DRC refine | routed human | routed full | routed refine |",
             "|---|---|---|---|---|---|---|---|---|---|---|---|"]
    for r in results:
        f, rf = r["full"].get("report") or {}, r["refine"].get("report") or {}

        def new_drc(k):
            h, o = r["human"].get("drc"), r[k].get("drc")
            if h is None or o is None:
                return "n/a"
            diffs = [f"{t}+{o.get(t, 0) - h.get(t, 0)}" for t in PLACEMENT_TYPES if o.get(t, 0) > h.get(t, 0)]
            return ", ".join(diffs) if diffs else "0"

        def rt(k):
            x = r[k].get("route")
            return f"{x['routed']}/{x['connections']}" if x else "-"

        def fullnote():
            notes = " ".join(f.get("notes", []))
            return " (=refine)" if "refine-mode result" in notes else (" (clr 0)" if "retried" in notes else "")

        lines.append(
            f"| {r['board']} | {f.get('movable', '?')} | {f.get('before', {}).get('hpwl_mm', 0):.0f} | "
            f"{f.get('after', {}).get('hpwl_mm', 0):.0f}{fullnote()} | {rf.get('after', {}).get('hpwl_mm', 0):.0f} | "
            f"{f.get('bounds', {}).get('lb_any_rotation_mm', 0):.0f} | "
            f"{f.get('before', {}).get('crossings', '?')}/{f.get('after', {}).get('crossings', '?')}/{rf.get('after', {}).get('crossings', '?')} | "
            f"{new_drc('full')} | {new_drc('refine')} | {rt('human')} | {rt('full')} | {rt('refine')} |")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--out", default=os.path.join(ROOT, "build/place-eval/run"))
    ap.add_argument("--place", default=os.path.join(ROOT, "build/release/src/place/tracemaker-place"))
    ap.add_argument("--tm", default=os.path.join(ROOT, "build/release/src/app/tracemaker"))
    ap.add_argument("--jobs", type=int, default=6)
    ap.add_argument("--place-threads", type=int, default=8)
    ap.add_argument("--route", action="store_true")
    ap.add_argument("--route-time", type=int, default=60)
    ap.add_argument("--truth", action="store_true")
    ap.add_argument("--no-drc", dest="drc", action="store_false")
    ap.add_argument("--reuse", action="store_true", help="reuse existing placement/route outputs")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    with cf.ThreadPoolExecutor(args.jobs) as ex:
        results = list(ex.map(lambda n: one(args, n), args.boards))
    json.dump(results, open(os.path.join(args.out, "summary.json"), "w"), indent=1)
    table = fmt_table(results)
    open(os.path.join(args.out, "summary.md"), "w").write(table + "\n")
    print(table)


if __name__ == "__main__":
    main()
