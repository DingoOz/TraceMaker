#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Ledger of the boards that are not yet routed 100 % (clean under the D106 judge) across the stack runs, so they can be continued with more time.

Runs read: bench/results/stack-<set>-s<N>[-g*] (set = srj19, srj20, tierA..tierD) and bench/results/full0100-tier<X> (stage 0 of the tiers).
Per (set, board): every attempt (run, seconds, unconnected, added errors, gate, clean), the best attempt (clean first, then fewest open,
no added error) and its saved search state, which the next continuation resumes from.

  bench/ledger.py                  write bench/results/stack-ledger.json and stack-ledger.md, print the counts
  bench/ledger.py --open SET       print the open boards of SET, one per line
"""
import argparse, json, pathlib, re, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
RES = ROOT / "bench/results"
STACK = re.compile(r"^stack-(srj19|srj20|tier[A-D])-s(\d+)(?:-[\w-]+)?$")
FULL = re.compile(r"^full0100-(tier[A-D])$")


# Boards whose own file makes a clean result impossible for any router: more time does not help, so they are not continued.
FIXTURE = {
    "tierA/HID_PID_controller": "setup trace_min is 6.4516 mm (0.254 mm x 25.4, a unit slip) while its net classes use 0.254 mm: KiCad flags every track as too narrow "
                                "(the reference routing has 199 track_width errors, the report cap) and a router that obeys it has no room to route anything",
}


def runs():
    out = []
    for d in sorted(RES.iterdir()):
        m = STACK.match(d.name)
        if m:
            out.append((m.group(1), int(m.group(2)), d))
            continue
        m = FULL.match(d.name)
        if m:
            out.append((m.group(1), 0, d))
    return sorted(out, key=lambda x: (x[0], x[1], x[2].name))


def score(a):
    """Lower is better: clean first, then no added error, then fewest connections open."""
    return (0 if a["clean"] else 1, 1 if a["added"] else 0, a["open"])


def build():
    led = {}
    for key, stage, d in runs():
        f = d / "boards.jsonl"
        if not f.exists():
            continue
        for line in f.read_text().splitlines():
            r = json.loads(line)
            if "clean" not in r:
                continue
            a = {"run": d.name, "stage": stage, "seconds": r.get("seconds", 0), "open": r.get("unconnected_after", 0), "added": r.get("added_errors", {}),
                 "gate": r.get("error_gate"), "clean": bool(r["clean"]), "routed": r.get("routed"), "connections": r.get("connections"),
                 "state": (d / "boards" / f"{r['board']}.kicad_pcb.state").exists()}
            e = led.setdefault(f"{key}/{r['board']}", {"set": key, "board": r["board"], "attempts": []})
            e["attempts"].append(a)
    for e in led.values():
        e["best"] = min(e["attempts"], key=lambda a: (score(a), -a["stage"]))
        e["clean"] = e["best"]["clean"]
        b = e["best"]
        # open: more time can still help. defect: a routing error was added (a bug, not a time problem). capped: fully routed, but the judge cannot
        # tell (D106 unknown gate): nothing to gain from time.
        e["state"] = "clean" if b["clean"] else "defect" if b["added"] else "open" if b["open"] else "capped"
        if e["state"] != "clean" and f"{e['set']}/{e['board']}" in FIXTURE:
            e["state"], e["note"] = "fixture", FIXTURE[f"{e['set']}/{e['board']}"]
        e["total_seconds"] = round(sum(a["seconds"] for a in e["attempts"]), 1)
    return led


def burndown(totals):
    """Boards not yet clean over time, per set and in total: (time, set, board, clean) of every attempt, in the order the results landed (mtime of the route's
    own record), and from it the count of boards without a clean attempt so far. Untested boards count as not clean, so the curve starts at the campaign total.
    Returns {"t0", "series": {set: [[t, not_clean], ...]}, "total": [[t, not_clean], ...]}."""
    ev = []
    for key, stage, d in runs():
        f = d / "boards.jsonl"
        if not f.exists():
            continue
        for line in f.read_text().splitlines():
            r = json.loads(line)
            if "clean" not in r:
                continue
            for suf in (".route.json", ""):
                g = d / "boards" / f"{r['board']}.kicad_pcb{suf}"
                if g.exists():
                    ev.append((g.stat().st_mtime, key, r["board"], bool(r["clean"])))
                    break
    ev.sort()
    if not ev:
        return {"t0": 0, "series": {}, "total": []}
    left = dict(totals)  # boards not yet clean per set
    done = set()
    series = {k: [[ev[0][0], v]] for k, v in totals.items()}
    total = [[ev[0][0], sum(totals.values())]]
    for t, key, board, ok in ev:
        if not ok or (key, board) in done:
            continue
        done.add((key, board))
        left[key] -= 1
        series[key].append([t, left[key]])
        total.append([t, sum(left.values())])
    now = time.time()
    for k in series:
        series[k].append([now, left[k]])
    total.append([now, sum(left.values())])
    return {"t0": ev[0][0], "series": series, "total": total}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--open")
    ap.add_argument("--write", action="store_true", default=True)
    a = ap.parse_args()
    led = build()
    if a.open:
        for k in sorted(led):
            if led[k]["set"] == a.open and led[k]["state"] == "open":
                print(led[k]["board"])
        return
    (RES / "stack-ledger.json").write_text(json.dumps(led, indent=1))
    sets = sorted({e["set"] for e in led.values()})
    lines = ["# Stack ledger: boards not yet clean", ""]
    for s in sets:
        es = [e for e in led.values() if e["set"] == s]
        op = sorted((e for e in es if not e["clean"]), key=lambda e: e["board"])
        cnt = {k: sum(1 for e in es if e["state"] == k) for k in ("clean", "open", "defect", "capped", "fixture")}
        lines += [f"## {s}: {cnt['clean']} of {len(es)} clean; {cnt['open']} open (continue with more time), {cnt['defect']} with an added error (bug), "
                  f"{cnt['capped']} fully routed but not judged clean (report cap), {cnt['fixture']} with a defective fixture", ""]
        if op:
            lines += ["| Board | State | Attempts | Best run | Open | Added errors | Gate | Seconds spent |", "|---|---|--:|---|--:|---|---|--:|"]
            for e in op:
                b = e["best"]
                lines.append(f"| {e['board']} | {e['state']} | {len(e['attempts'])} | {b['run']} | {b['open']} | {b['added'] or ''} | {b['gate']} | {e['total_seconds']:.0f} |")
            lines.append("")
        print(f"{s}: {cnt['clean']}/{len(es)} clean, open {cnt['open']}, defect {cnt['defect']}, capped {cnt['capped']}" + (f", fixture {cnt['fixture']}" if cnt["fixture"] else ""))
    (RES / "stack-ledger.md").write_text("\n".join(lines) + "\n")


if __name__ == "__main__":
    main()
