#!/usr/bin/env python3
"""Summarise viastudy outputs (build/via-layer-study/out/*.json) into tables for the notes."""
import json, pathlib, re, statistics, sys
from collections import Counter

ROOT = pathlib.Path("/home/dingo/Programming/TraceMaker-speed")
OUT = ROOT / "build/via-layer-study/out"
RES = ROOT / "bench/results"
boards = sys.argv[1:] or [p.stem for p in sorted(OUT.glob("*.json")) if not p.stem.startswith("test")]


def run_failures(board):
    for t in "BCD":
        f = RES / f"rel0120-tier{t}/boards/{board}.kicad_pcb.route.json"
        if f.exists():
            d = json.loads(f.read_text())
            return d.get("failures", []), d.get("variant_name", ""), d.get("pitch_mm")
    return [], "", None


def why_class(w):
    w = w or ""
    if "boxed in (target)" in w.split(";")[0]: return "boxed in (target)"
    if "boxed in" in w.split(";")[0]: return "boxed in (source)"
    if "out of budget" in w: return "out of budget"
    if "search budget" in w: return "search budget"
    if "no path in window" in w: return "no path in window"
    return w.split(":")[0][:40] or "?"


def orig_class(s):
    s = s.lower()
    for k in ["ripped up by", "boxed in (target)", "boxed in", "out of budget", "search budget", "no path in window", "rip limit", "exact check", "not attempted", "rolled back"]:
        if k in s: return k
    return s[:50]


tot = Counter()
rows = []
gap_open, gap_conn = [], []
examples = []
why_big = Counter()
orig_tot = Counter()
vias_A = Counter()
vias_rescue = Counter()
for b in boards:
    p = OUT / f"{b}.json"
    if not p.exists():
        continue
    d = json.loads(p.read_text())
    fails, vname, rpitch = run_failures(b)
    oc = Counter(orig_class(f.split("  (", 1)[1] if "  (" in f else f) for f in fails)
    orig_tot.update(oc)
    C = d["connections"]
    n = len(C)
    c = Counter()
    for x in C:
        e = x["exp"]
        A = e.get("A", {})
        if x.get("dead_end"): c["dead_end"] += 1
        if A.get("routed"):
            c["A"] += 1
            vias_A[min(A.get("vias", A.get("vias_net", 0)), 6)] += 1
            continue
        c["Afail"] += 1
        r = {k: e.get(k, {}).get("routed", False) for k in ["V05", "V1", "V2", "T2", "VT2", "BIG", "CHEAP", "FINE"]}
        for k, v in r.items():
            if v: c[k] += 1
        if r["V05"] or r["V1"] or r["V2"]: c["anyV"] += 1
        if (r["V05"] or r["V1"] or r["V2"]) and not r["T2"]: c["Vonly"] += 1
        if r["T2"] and not (r["V05"] or r["V1"] or r["V2"]): c["Tonly"] += 1
        if r["VT2"] and not (r["V2"] or r["T2"]): c["VTonly"] += 1
        if not any(r.values()): c["none"] += 1
        if r["BIG"] or r["CHEAP"] or r["FINE"]: c["search"] += 1
        sw = e.get("SWAP1")
        if sw and "all_ok" in sw:
            c["swap_tried"] += 1
            if sw["all_ok"]: c["swap_ok"] += 1
        if sw and "skipped" in sw: c["swap_skipped"] += 1
        why_big[why_class(e.get("BIG", {}).get("why", ""))] += 1
        for k in ["V05", "V1", "V2", "BIG", "CHEAP", "FINE"]:
            if r[k]:
                vias_rescue[(k, min(e[k].get("vias", e[k].get("vias_net", 0)), 6))] += 1
        examples.append((b, x))
        # open-end geometry
    for x in C:
        for en in x["ends"]:
            pass
    for en in d["control"]:
        g = en["via_gap_nearest_mm"]
        g = 99 if g < 0 else g
        if en.get("dense_pitch") is None:
            continue
        (gap_open if en["open"] else gap_conn).append(g)
    c["n"] = n
    c["open"] = d["open"]
    tot.update(c)
    rows.append((b, d["copper_layers"], d["pitch_mm"], d["open"], n, c, vname, rpitch, oc))

cols = ["A", "Afail", "dead_end", "V05", "V1", "V2", "T2", "VT2", "anyV", "Vonly", "Tonly", "BIG", "CHEAP", "FINE", "search", "none", "swap_tried", "swap_ok"]
print("| board | L | pitch | open | sampled | " + " | ".join(cols) + " |")
print("|---|--:|--:|--:|--:|" + "--:|" * len(cols))
for b, L, pt, op, n, c, vn, rp, oc in rows:
    print(f"| {b} | {L} | {pt} | {op} | {n} | " + " | ".join(str(c.get(k, 0)) for k in cols) + " |")
print(f"| total | | | {tot['open']} | {tot['n']} | " + " | ".join(str(tot.get(k, 0)) for k in cols) + " |")
print()
print("original run failure reasons:", dict(orig_tot))
for b, L, pt, op, n, c, vn, rp, oc in rows:
    print(" ", b, vn, rp, dict(oc))
print("BIG why for A-fails:", dict(why_big))
print("vias in A routes:", sorted(vias_A.items()))
print("vias in rescue routes:", sorted(vias_rescue.items()))


def q(v):
    v = sorted(v)
    if not v: return "-"
    return f"n={len(v)} p25={v[len(v)//4]:.2f} med={statistics.median(v):.2f} <0.25:{sum(1 for x in v if x < .25)/len(v):.0%} <0.5:{sum(1 for x in v if x < .5)/len(v):.0%} <1:{sum(1 for x in v if x < 1)/len(v):.0%}"


print("nearest foreign via gap (mm), pads of dense parts holding an open end: open pads", q(gap_open))
print("                                                                   connected pads", q(gap_conn))

# ---- extra: nearest foreign via at open ends, own-part vs through; per board
print()
print("| board | open ends at dense parts | nearest foreign via gap median (mm) | nearest is own-part net | connected pads of same parts: median gap |")
print("|---|--:|--:|--:|--:|")
for b, L, pt, op, n, c, vn, rp, oc in rows:
    d = json.loads((OUT / f"{b}.json").read_text())
    og, cg, own = [], [], 0
    for en in d["control"]:
        if en.get("dense_pitch") is None: continue
        g = en["via_gap_nearest_mm"]; g = 99 if g < 0 else g
        if en["open"]:
            og.append(g)
            go, gt = en["via_gap_nearest_ownpart_mm"], en["via_gap_nearest_through_mm"]
            go = 99 if go < 0 else go; gt = 99 if gt < 0 else gt
            if go <= gt: own += 1
        else: cg.append(g)
    if og:
        print(f"| {b} | {len(og)} | {statistics.median(og):.2f} | {own}/{len(og)} | {statistics.median(cg) if cg else float('nan'):.2f} (n={len(cg)}) |")
# rescue route via counts
print()
for k in ["V05", "V1", "V2", "T2", "VT2", "BIG", "CHEAP", "FINE"]:
    cnt = Counter()
    for b, x in examples:
        e = x["exp"].get(k, {})
        if e.get("routed"):
            cnt[e.get("vias", "?")] += 1
    print(k, "rescued routes by via count:", sorted(cnt.items(), key=lambda t: str(t[0])))
