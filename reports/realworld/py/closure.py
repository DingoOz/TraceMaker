#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Closure loop: keep removing what stops the routing until it is complete.

  closure.py BOARD[+planes] NAME [--rounds N] [--work W] [--move-work W] [--max-tier T3] [route options...]

Each round routes (warm-started from the round before), has KiCad judge the copper on the board's own rules and reads what is still open. The cause is then
removed, the cheapest and least harmful first:
  1. relax the rules of the nets that are open, and of the SIGNAL nets crowding their end points, by one tier (T0 own -> T1 -> T2 -> T3), only those nets;
  2. the power nets that are open are relaxed only when no signal net can be relaxed any further (power has the least margin: it goes last);
  3. when nothing is left to relax, or relaxing did not help twice, a part next to an open end point is moved away from it (0.25, 0.5, 1.0 mm; one trial
     per candidate, in parallel); the move is kept only if KiCad then shows fewer open connections and no new courtyard or hole error.
Every step is a transaction: a round that opens more connections than the best so far is discarded. What was done is written to NAME/closure.json (rounds,
nets and their tier, parts moved) for the compromise ledger, the safety simulation and the dev page.
"""
from __future__ import annotations

import argparse
import concurrent.futures as cf
import json
import math
import os
import pathlib
import random
import re
import subprocess
import sys
import time

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import kgeom
import combine as combine_mod
import ledger
import pcbtext
import rules

ROOT = pathlib.Path(__file__).resolve().parents[3]
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = pathlib.Path(os.environ["TM_BIN"]) if os.environ.get("TM_BIN") else ROOT / "build/release/src/app/tracemaker"  # TM_BIN: a frozen copy of the router for a long run
PLACEMENT_ERRORS = {"courtyards_overlap", "overlapping_items", "pth_inside_courtyard", "npth_inside_courtyard", "hole_to_hole", "holes_co_located", "copper_edge_clearance", "hole_clearance"}
TIER_ORDER = ["T0", "T1", "T2", "T3"]
NEIGHBOUR_MM = 1.2   # a signal net this close to an open end point shares its relaxation: a gap needs both sides to give
MOVE_RADIUS_MM = 3.0  # parts this close to an open end point are candidates to move away
STEPS_MM = (0.25, 0.5)  # 1.0 mm dropped: it never won a trial (DronPi IC11 0.25, E3 C11 0.25) and trials are the slowest step


T0 = time.time()


def log(msg: str) -> None:
    print(time.strftime("%H:%M:%S"), msg, flush=True)


THREADS = 8      # router threads per route call (--threads)
SEED = 0         # router seed (--seed): a different seed is a different portfolio, so a stuck board can be tried again
TRIAL_JOBS = 4   # trials run side by side (--trial-jobs)
SHUFFLES = 3     # seeded shuffles per reach in the package reroute (--package-shuffles)
REACHES = (4, 10)  # pins either way of an open pin whose nets the package reroute rips up (--package-reach)
VIEW: dict = {}  # --view-port: {"port": base, "host": ..., "token": ...}; the live viewer of the router, one port per concurrent router


def route(text: str, d: pathlib.Path, warm: pathlib.Path | None, work: int, extra: list[str], strict: list[str], slot: int = 0) -> tuple[pathlib.Path, str]:
    d.mkdir(parents=True, exist_ok=True)
    (d / "relaxed.kicad_pcb").write_text(text)
    cmd = [str(TM), "route", str(d / "relaxed.kicad_pcb"), "-o", str(d / "routed_relaxed.kicad_pcb"), "--work", str(work), "--threads", str(THREADS), "--no-gpu", "--no-kb"] + (["--seed", str(SEED)] if SEED else []) + extra
    if strict:
        cmd += ["--no-neck-nets", ",".join(n for n in strict if "," not in n)]
    if warm:
        cmd += ["--warm-pcb", str(warm)]
    out_f = d / "routed_relaxed.kicad_pcb"
    for attempt in range(6):  # a viewer port another run holds is a failure of the viewer, not of the routing: try other ports, the last time none
        view = ["--view", "--view-port", str(VIEW["port"] + slot + 40 * attempt), "--view-host", VIEW["host"], "--view-token", VIEW["token"], "--no-freeze"] if VIEW and attempt < 5 else []
        with open(d / "route.log", "w") as f:
            subprocess.run(cmd + view, stdout=f, stderr=subprocess.STDOUT)
        if out_f.exists() or "Address already in use" not in (d / "route.log").read_text():
            break
    line = next((l for l in (d / "route.log").read_text().splitlines() if l.startswith("routed ")), "no result")
    return d / "routed_relaxed.kicad_pcb", line


HARD: set[str] = set()  # nets routed pad to pad with their pours taken out of the router's board (put back for KiCad)


def judge(routed: pathlib.Path, src: str, d: pathlib.Path, hard=None) -> dict:
    """KiCad's verdict on the routed copper under the board's own rules: open connections (per net, with where) and the placement errors."""
    own = d / "routed.kicad_pcb"
    own.write_text(pcbtext.add_zones(pcbtext.restore_rules(routed.read_text(), src), src, HARD if hard is None else hard))
    drc = ledger.kicad_drc(own, d / "routed.drc.json")
    opens: dict[str, list] = {}
    for u in drc.get("unconnected_items", []):
        pts = [(i["pos"]["x"], i["pos"]["y"]) for i in u["items"] if not i["description"].startswith("Zone")]
        net = next((m.group(1) for i in u["items"] for m in [re.search(r"\[([^\]]*)\]", i["description"])] if m), "")
        opens.setdefault(net, []).extend(pts or [(u["items"][0]["pos"]["x"], u["items"][0]["pos"]["y"])])
    placement = sum(1 for v in drc.get("violations", []) if v["type"] in PLACEMENT_ERRORS)
    viol = sum(1 for v in drc.get("violations", []) if v["type"] not in ledger.IGNORE)  # what is wrong with the copper under the board's own rules
    return {"open": len(drc.get("unconnected_items", [])), "opens": opens, "placement": placement, "violations": viol}


def seg_dist(px, py, t) -> float:
    dx, dy = t.x2 - t.x1, t.y2 - t.y1
    L = dx * dx + dy * dy
    u = 0.0 if L == 0 else max(0.0, min(1.0, ((px - t.x1) * dx + (py - t.y1) * dy) / L))
    return math.hypot(px - (t.x1 + u * dx), py - (t.y1 + u * dy)) - t.w / 2


def neighbours(b: kgeom.Board, points: list[tuple[float, float]], net: str) -> set[str]:
    out = set()
    names = b.nets
    for (x, y) in points:
        for t in b.tracks:
            if names.get(t.net, "") != net and seg_dist(x, y, t) <= NEIGHBOUR_MM:
                out.add(names.get(t.net, ""))
        for v in b.vias:
            if names.get(v.net, "") != net and math.hypot(x - v.x, y - v.y) - v.size / 2 <= NEIGHBOUR_MM:
                out.add(names.get(v.net, ""))
        for part in b.parts:
            for p in part.pads:
                if p.netname and p.netname != net and math.hypot(x - p.x, y - p.y) - max(p.w, p.h) / 2 <= NEIGHBOUR_MM:
                    out.add(p.netname)
    return out - {""}


def candidates(b: kgeom.Board, opens: dict[str, list]) -> list[tuple[str, float, float, float]]:
    """(ref, ux, uy, nearness): unlocked parts near an open end point that do not own it, with the unit vector pointing away from the end point."""
    cand: dict[str, tuple] = {}
    for net, pts in opens.items():
        for (x, y) in pts:
            owners = {p.ref for part in b.parts for p in part.pads if math.hypot(p.x - x, p.y - y) < 0.05}
            for part in b.parts:
                if part.locked or part.ref in owners or part.ref.startswith(("H", "MH", "FID")):
                    continue
                d = min((math.hypot(p.x - x, p.y - y) for p in part.pads), default=1e9)
                if d <= MOVE_RADIUS_MM:
                    ux, uy = part.x - x, part.y - y
                    n = math.hypot(ux, uy) or 1.0
                    if part.ref not in cand or d < cand[part.ref][3]:
                        cand[part.ref] = (part.ref, ux / n, uy / n, d)
    return sorted(cand.values(), key=lambda c: (c[3], c[0]))


def designer_widths(pcb: pathlib.Path, power: set, src: str) -> dict[str, float]:
    """Per power net, the track width the designer used on most of its length (the length-weighted median), where it is above the net's class width.
    This stands in for a current profile, which the test data does not have: it is the designer's own answer to 'how wide must this net be'."""
    b = kgeom.load(str(pcb))
    cls_w = {}
    for m in re.finditer(r"\(net_class .*?\n  \)", src, flags=re.S):
        w = re.search(r"\(trace_width ([0-9.]+)\)", m.group(0))
        for l in m.group(0).split("\n"):
            if (ad := rules.ADD_NET.match(l)) and w:
                cls_w[rules._name(ad.group(1))] = float(w.group(1))
    default_w = float(re.search(r"\(trace_width ([0-9.]+)\)", src).group(1)) if re.search(r"\(trace_width ([0-9.]+)\)", src) else 0.25
    per: dict[str, list] = {}
    for t in b.tracks:
        n = b.nets.get(t.net, "")
        if n in power:
            per.setdefault(n, []).append((round(t.w, 3), math.hypot(t.x2 - t.x1, t.y2 - t.y1)))
    out = {}
    for n, items in per.items():
        items.sort()
        half, acc = sum(l for _, l in items) / 2, 0.0
        for w, l in items:
            acc += l
            if acc >= half:
                break
        if w > cls_w.get(n, default_w) + 1e-6:
            out[n] = w
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("board")
    ap.add_argument("name")
    ap.add_argument("--rounds", type=int, default=12)
    ap.add_argument("--work", type=int, default=20_000_000)
    ap.add_argument("--move-work", type=int, default=10_000_000)
    ap.add_argument("--move-trials", type=int, default=5, help="candidate part moves tried per move round")
    ap.add_argument("--max-tier", default="T3", choices=TIER_ORDER[1:])
    ap.add_argument("--no-moves", action="store_true")
    ap.add_argument("--threads", type=int, default=8, help="router threads per route call")
    ap.add_argument("--trial-jobs", type=int, default=4, help="trials (part moves, window reroutes) run side by side")
    ap.add_argument("--no-shove", action="store_true", help="skip the shove step: after a relaxation let a net through, push its copper back to the board's rules (bending and moving tracks) and keep that if nothing opens")
    ap.add_argument("--shove-cut", type=float, default=0.5, help="the shove cuts tracks into pieces of at most this length (mm) so they can bend")
    ap.add_argument("--shove-work", type=int, default=10_000_000)
    ap.add_argument("--no-hard", action="store_true", help="do not take the pour of an open net out of the router's board (the first remedy for pour islands)")
    ap.add_argument("--no-window", action="store_true", help="skip the window reroute: rip up what is near the open end points and route the open nets first")
    ap.add_argument("--no-escape", action="store_true", help="do not switch on the escape plan with long corridors when open nets end at pins of a dense package")
    ap.add_argument("--no-copper-graphics", action="store_true", help="do not switch on --soft-copper-graphics when open nets remain")
    ap.add_argument("--no-mask", action="store_true", help="do not switch on --soft-mask-graphics when open nets remain")
    ap.add_argument("--resume", help="start from the final state of an earlier run of this board (its routed.kicad_pcb, tiers, hard pours, moves): no rounds, only the finishing phase")
    ap.add_argument("--finish-passes", type=int, default=3, help="passes of the finishing phase")
    ap.add_argument("--deadline-min", type=float, default=0, help="after this many minutes no new round or remedy is started: the best state so far is written (0 = no deadline)")
    ap.add_argument("--no-power-neck", action="store_true", help="power nets are never necked (kept at their class width), in every round, not only in the finishing phase")
    ap.add_argument("--seed", type=int, default=0, help="router seed for every route call (0 = the router default)")
    ap.add_argument("--no-combine", action="store_true", help="skip the combine step: a net that is open here but complete in another judged state of the same board (another run, seed or remedy) is taken over from it")
    ap.add_argument("--no-package", action="store_true", help="skip the package reroute: the nets of the pins around an open pin of a dense package are ripped up whole and routed again as a group, in the order of the pins, with a widening scope")
    ap.add_argument("--package-shuffles", type=int, default=3, help="seeded random orders tried per reach by the package reroute (a cheap search: hundreds are affordable)")
    ap.add_argument("--package-reach", default="4,10", help="comma list of reaches (pins either way of an open pin) for the package reroute")
    ap.add_argument("--no-bundle", action="store_true", help="skip the bundle reroute: the signal nets of the dense package round an open pin are ripped up and placed again as bundles of parallel lanes (router --bundle-escape)")
    ap.add_argument("--round0", default="", help="extra router arguments for the first from-scratch route only, e.g. \"--first-nets +3V3,Net-(C12-Pad1)\" (nets with parentheses must be one word: no spaces)")
    ap.add_argument("--region-crossover", action="store_true", help="the combine step also tries region crossovers: the nets of a donor state with copper within 3, 6 or 10 mm of the open end points replace the best state's (with the nets their copper touches)")
    ap.add_argument("--package-via-drops", action="store_true", help="the package reroute also tries every order with --via-drops (pin-to-via drops off the lattice)")
    ap.add_argument("--no-finish", action="store_true", help="skip the finishing phase after the rounds (fine finish and window reroute on the best state)")
    ap.add_argument("--no-fine", action="store_true", help="skip the fine-lattice finish: each open net alone, its rules and its neighbours' at the loosest tier, on a lattice of 10-20 um")
    ap.add_argument("--no-effort", action="store_true", help="skip the last resort before stopping: one round at 5x the work")
    ap.add_argument("--widths-from", help="a routed .kicad_pcb (the designer's): its power nets are routed at the width the designer used on most of their length, never necked, never below the class width")
    ap.add_argument("--view-port", type=int, help="stream the routing live: the main router on this port, the part-move trials on the ports after it (slot 1..N)")
    ap.add_argument("--view-host", default="0.0.0.0", help="viewer bind address (a token protects it)")
    a, extra = ap.parse_known_args()
    board, planes = a.board.removesuffix("+planes"), a.board.endswith("+planes")
    if planes:
        extra = ["--soft-zones"] + extra
    global THREADS, TRIAL_JOBS, SEED, SHUFFLES, REACHES
    THREADS, TRIAL_JOBS, SEED = a.threads, a.trial_jobs, a.seed
    SHUFFLES, REACHES = a.package_shuffles, tuple(int(x) for x in a.package_reach.split(","))
    out = ROOT / "reports/realworld/boards" / board / a.name
    out.mkdir(parents=True, exist_ok=True)
    if a.view_port:
        import secrets
        VIEW.update(port=a.view_port, host=a.view_host, token=secrets.token_urlsafe(16))
        (out / "view.json").write_text(json.dumps({"port": a.view_port, "trial_ports": [a.view_port + 1, a.view_port + 4 * 8], "token": VIEW["token"]}))
        log(f"live viewer: http://<host>:{a.view_port}/?token={VIEW['token']}  (trials on the ports after it)")
    src = ((ROOT / "reports/realworld/boards" / board / "planes.kicad_pcb") if planes else (FIX / board / "unrouted.kicad_pcb")).read_text()
    own = rules.own_tier(src)
    tiers = dict(rules.TIERS)
    tiers["T0"] = own
    if own["vmin"] < own["via"] or own["dmin"] < own["drill"]:
        # Tier V, between the board's own rules and T1: everything as the board has it, but vias of the board's own MINIMUM size (EtherCAT: 0.4/0.3 against the class's
        # 0.6/0.4; the designer used 0.4/0.3 vias throughout). No rule is broken; only the fab margin of the via ring shrinks.
        tiers["V"] = dict(own, via=min(own["via"], own["vmin"]), drill=min(own["drill"], own["dmin"]))
        TIER_ORDER[:] = ["T0", "V", "T1", "T2", "T3"]
    power = set(rules.power_nets(src))
    HARD.clear()
    mask_graphics = bool(re.search(r"\(fp_\w+[^\n]*\(layer [FB]\.Mask\)", src))  # footprint graphics on a mask layer (KiCad 5 files: one record per line)
    mask_on = False
    copper_graphics = bool(re.search(r"\n\s*\(gr_(?:poly|line|arc|circle|curve|rect)[^\n]*\(layer [FB]\.Cu\)", src))  # net-less copper drawn as board graphics
    copper_on = False
    escape_on = False
    escape_tried = False
    zone_nets = {pcbtext.zone_net(src[a:b]) for h, a, b in pcbtext.toplevel(src) if h == "zone"} - {""}
    nets_all = rules.all_nets(src)
    base_drc = ROOT / "reports/realworld/boards" / board / ("planes.drc.json" if planes else "unrouted.drc.json")
    base_placement = 0
    if base_drc.exists():
        base_placement = sum(1 for v in json.loads(base_drc.read_text()).get("violations", []) if v["type"] in PLACEMENT_ERRORS)
    max_idx = TIER_ORDER.index(a.max_tier)
    assign: dict[str, int] = {}   # net -> tier index (1..3); absent = the board's own rules
    moves: dict[str, list] = {}   # ref -> [dx, dy] total
    rounds: list[dict] = []
    best = None
    warm = None
    stall = 0
    cur_src = src
    labels: list[str] = []

    def write_state():
        (out / "stages.txt").write_text(",".join(labels) + "\n")
        (out / "closure.json").write_text(json.dumps({"rounds": rounds, "assign": {n: TIER_ORDER[i] for n, i in sorted(assign.items())}, "moves": moves,
                                                      "power": sorted(power), "hard": sorted(HARD), "tiers": tiers, "max_tier": a.max_tier,
                                                      "fine_done": [sorted(x) for x in fine_done]}, indent=1))

    widths: dict[str, float] = {}
    if a.widths_from:
        widths = designer_widths(pathlib.Path(a.widths_from), power, src)
        log(f"{board[:24]} designer widths for {len(widths)} power nets: " + ", ".join(f"{n} {w}" for n, w in sorted(widths.items())[:14]))

    def text_for(assign_now: dict[str, int], base: str, neck_power: bool = True) -> tuple[str, list[str]]:
        neck_power = neck_power and not a.no_power_neck
        t = rules.apply_nets(pcbtext.remove_zones(base, HARD), {n: TIER_ORDER[i] for n, i in assign_now.items()}, tiers, widths)
        # a net with a stated width is never necked; in the finishing phase power nets are not either (closure-22: +3V3 necked to 0.15 mm at the very end failed the IPC check)
        return t, [n for n in nets_all if n not in assign_now or n in widths or (not neck_power and n in power)]

    raised_last: set[str] = set()   # nets that took one more tier in the last remedy
    late = lambda: bool(a.deadline_min) and time.time() - T0 > a.deadline_min * 60  # past the deadline: nothing new is started
    fine_done: set = set()          # open-net sets the fine finish has already been tried on (a repeat costs ten minutes and changes nothing)
    window_cap = [80]               # items the window reroute may rip per net; the finishing phase raises it from the second pass on
    shove_fails = 0                 # shoves discarded in a row: after two the step is skipped (it costs a route and a judging each time)
    def window_reroute() -> bool:
        """For each open net (up to 6): the copper near its open end points (whole pieces of nets, not power or ground trees; the largest radius of 2, 4, 8 mm that
        rips at most 80 items) is ripped up and that net is routed FIRST through the cleared window, the others around it. Kept only if KiCad then shows
        fewer open connections."""
        if a.no_window or late() or any(r_.get("kind") == "window" and r_.get("round") == k for r_ in rounds):
            return False
        names = [n for n in j["opens"] if n and "," not in n][:6]
        if not names:
            return False
        bp = kgeom.load(str(best["routed"]))
        ids = {nm: i_ for i_, nm in bp.nets.items()}
        base_text = best["routed"].read_text()
        tt, st = text_for(assign, cur_src, neck_power=False)

        def one(ri: int, name: str):
            centers = j["opens"][name]
            keep_ids = {ids[name]} if name in ids else set()
            pick = None
            for radius in ((14.0, 10.0, 6.0, 3.0) if window_cap[0] > 100 else (8.0, 4.0, 2.0)):
                stripped, nrem, lost = pcbtext.strip_window(base_text, centers, radius, keep_ids, max_items=max(60, window_cap[0] // 2))
                pick = (radius, stripped, nrem)
                if nrem <= window_cap[0]:
                    break
            radius, stripped, nrem = pick
            wd = out / f"win-{k:02d}-{ri}"
            wd.mkdir(exist_ok=True)
            (wd / "warm.kicad_pcb").write_text(stripped)
            rpt, ln = route(tt, wd, wd / "warm.kicad_pcb", a.work, extra + ["--first-nets", name], st, 1 + ri)  # the ripped nets need room to come back: the full round budget
            jj = judge(rpt, cur_src, wd)
            return {"net": name, "radius": radius, "removed": nrem, "opens": jj["opens"], "open": jj["open"], "violations": jj["violations"], "placement": jj["placement"], "routed": rpt}

        res_w = []
        with cf.ThreadPoolExecutor(TRIAL_JOBS) as ex:
            futs = [ex.submit(one, ri, n) for ri, n in enumerate(names)]
            for f in cf.as_completed(futs):
                try:
                    res_w.append(f.result())
                except Exception as e:
                    log(f"window trial failed: {e}")
        rounds.append({"round": k, "kind": "window", "trials": [{x: r_[x] for x in ("net", "radius", "removed", "open", "violations")} for r_ in sorted(res_w, key=lambda r_: r_["net"])]})
        ok_w = [r_ for r_ in res_w if r_["open"] < best["open"] and r_["placement"] <= max(base_placement, best["placement"])]
        log(f"{board[:24]} round {k}: window reroute: " + ", ".join(f"{r_['net'][:14]} r{r_['radius']:g}: {r_['open']} open (ripped {r_['removed']})" for r_ in sorted(res_w, key=lambda r_: r_["net"])))
        write_state()
        if not ok_w:
            return False
        w_ = min(ok_w, key=lambda r_: (r_["open"], r_["violations"], r_["removed"]))
        log(f"{board[:24]} window reroute of {w_['net']} kept: {best['open']} -> {w_['open']} open")
        best.update(open=w_["open"], violations=w_["violations"], placement=w_["placement"], routed=w_["routed"], opens=w_["opens"], dir=w_["routed"].parent, hard=set(HARD), assign=dict(assign))
        return True

    def combine() -> bool:
        """Cross-run effort: the judged states of the board's other runs (and of this run's own stages) close different nets. For each open net that another
        state has complete, the crossover of the two (combine.merge_closure: that net comes from the donor with every net its copper touches, the rest stays) is
        ranked by the open connections it should have (the best state's outside the taken nets plus the donor's inside); the best 8 get the router on what is
        still open (those nets first) and KiCad decides."""
        if a.no_combine or late() or any(r_.get("kind") == "combine" and r_.get("round") == k for r_ in rounds):
            return False
        names = [n for n in j["opens"] if n and "," not in n]
        if not names:
            return False
        base_text = best["routed"].read_text()
        bd = best["dir"] / "routed.drc.json"
        own = (combine_mod.drc_open_counts(bd) if bd.exists() else None) or {n: max(1, len(j["opens"][n]) // 2) for n in names}
        pool = combine_mod.donors(out.parent, best["routed"], moves, 200)
        region_pool = {(o_, f_) for o_, f_, _ in pool[:40]}  # the region crossovers are dearer: only the 40 best donors
        seen, cand = set(), []
        for o, f, cnt in pool:
            try:
                dtext = f.read_text()
            except OSError:
                continue
            for n in names:
                if n in cnt:
                    continue
                seed_sets = [[n]]
                if a.region_crossover and (o, f) in region_pool:  # a region of the donor: every net with copper within R mm of the open end points, not just the open net
                    pts = [pt for q in names for pt in j["opens"].get(q, [])]
                    for rad in (3.0, 6.0, 10.0):
                        near = combine_mod.nets_near(dtext, pts, rad)
                        seeds = sorted({q for q in near if q in cnt or True} | {n})
                        if len(seeds) > 1 and seeds not in seed_sets:
                            seed_sets.append(seeds)
                for seeds in seed_sets:
                    r_ = combine_mod.merge_closure(base_text, dtext, seeds)
                    if r_ is None or hash(r_[0]) in seen:
                        continue
                    seen.add(hash(r_[0]))
                    est = sum(v for q, v in own.items() if q not in r_[1]) + sum(cnt.get(q, 0) for q in r_[1])
                    cand.append((est, len(r_[1]), o, str(f), n if len(seeds) == 1 else f"{n}@{len(seeds)}", r_, cnt))
        cand = sorted(cand, key=lambda t: (t[0], t[1], t[2], t[3], t[4]))
        cand = [c for c in cand if c[0] <= best["open"] + 1][:8]
        if not cand:
            log(f"{board[:24]} round {k}: combine: no crossover with another state should leave fewer than {best['open']} open ({len(pool)} states, {len(seen)} crossovers)")
            return False
        tt, st = text_for(assign, cur_src, neck_power=False)

        def one(ti: int, c):
            est, ntaken, o, f, n, (text, taken), cnt = c
            wd = out / f"comb-{k:02d}-{ti}"
            wd.mkdir(exist_ok=True)
            (wd / "warm.kicad_pcb").write_text(text)
            still = sorted({q for q in own if q not in taken} | {q for q in taken if cnt.get(q, 0)})
            direct = judge(wd / "warm.kicad_pcb", cur_src, wd)  # the crossover as it is: nothing ripped, nothing routed
            rpt, jj = wd / "warm.kicad_pcb", direct
            if still and direct["open"] > 0:  # then the router on what is still open (it may rip, so it is a second candidate, kept only if it is better)
                wr = wd / "route"
                rd_, ln = route(tt, wr, wd / "warm.kicad_pcb", a.work, extra + ["--first-nets", ",".join(q for q in still if "," not in q)], st, 1 + ti)
                j2 = judge(rd_, cur_src, wr)
                if j2["open"] < direct["open"]:
                    rpt, jj = rd_, j2
            return {"net": n, "from": pathlib.Path(f).parent.relative_to(out.parent).as_posix(), "taken": ntaken, "est": est, "opens": jj["opens"], "open": jj["open"], "violations": jj["violations"], "placement": jj["placement"], "routed": rpt}

        res_c = []
        with cf.ThreadPoolExecutor(TRIAL_JOBS) as ex:
            futs = [ex.submit(one, ti, c) for ti, c in enumerate(cand)]
            for f in cf.as_completed(futs):
                try:
                    res_c.append(f.result())
                except Exception as e:
                    log(f"combine trial failed: {e}")
        rounds.append({"round": k, "kind": "combine", "trials": [{x: r_[x] for x in ("net", "from", "taken", "est", "open", "violations")} for r_ in sorted(res_c, key=lambda r_: (r_["net"], r_["from"]))]})
        log(f"{board[:24]} round {k}: combine: " + ", ".join(f"{r_['net'][:14]} from {r_['from']}: {r_['open']} open (est {r_['est']}, {r_['taken']} nets taken)" for r_ in sorted(res_c, key=lambda r_: (r_["net"], r_["from"]))))
        write_state()
        ok_c = [r_ for r_ in res_c if r_["open"] < best["open"] and r_["placement"] <= max(base_placement, best["placement"])]
        if not ok_c:
            return False
        w_ = min(ok_c, key=lambda r_: (r_["open"], r_["violations"], r_["taken"]))
        log(f"{board[:24]} combine: {w_['net']} taken from {w_['from']}: {best['open']} -> {w_['open']} open")
        best.update(open=w_["open"], violations=w_["violations"], placement=w_["placement"], routed=w_["routed"], opens=w_["opens"], dir=w_["routed"].parent, hard=set(HARD), assign=dict(assign))
        return True

    package_done: set = set()       # open-net sets the package reroute has been tried on
    def package_reroute() -> bool:
        """The fix a designer makes by eye: not the neighbourhood of the open end point, but the whole group of pins of the dense package around it. The nets of
        the pins within a reach of 4 and of 10 pins either way of the open pins (along the package's perimeter) are ripped up whole, wherever they run, and routed
        again ONE AT A TIME (--only-net: nothing placed is ripped, so the order alone decides who gets which channel) in several orders: round the package (the order
        a bundle leaves it in), the open nets first, reversed, and three seeded shuffles. Kept only if KiCad then shows fewer open connections. (Routing the whole group
        at once with --first-nets-ordered and the escape plan ended at 9-15 open on EtherCAT, against 3 before: the router does not build the comb from scratch.)"""
        if a.no_package or late() or any(r_.get("kind") == "package" and r_.get("round") == k for r_ in rounds):
            return False
        names_open = frozenset(n for n in j["opens"] if n)
        if not names_open or names_open in package_done:
            return False
        bq = kgeom.load(str(best["routed"]))
        ids = {nm: i_ for i_, nm in bq.nets.items()}
        pick = None
        for part in bq.parts:
            if len(part.pads) < 16:
                continue
            pitch_p = min((math.hypot(p1.x - p2.x, p1.y - p2.y) for i1, p1 in enumerate(part.pads) for p2 in part.pads[i1 + 1:] if math.hypot(p1.x - p2.x, p1.y - p2.y) > 0.05), default=9.0)
            if pitch_p > 0.65:
                continue
            hits = [i_ for i_, pd in enumerate(part.pads) if any(math.hypot(pd.x - x_, pd.y - y_) < 0.05 for pts in j["opens"].values() for (x_, y_) in pts)]
            if hits and (pick is None or len(hits) > len(pick[1])):
                pick = (part, hits)
        if pick is None:
            return False
        package_done.add(names_open)
        part, hits = pick
        cx = sum(p.x for p in part.pads) / len(part.pads)
        cy = sum(p.y for p in part.pads) / len(part.pads)
        ring = sorted(range(len(part.pads)), key=lambda i_: math.atan2(part.pads[i_].y - cy, part.pads[i_].x - cx))  # the pins in the order round the package
        pos = {i_: n_ for n_, i_ in enumerate(ring)}
        skip = set(power) | set(HARD) | zone_nets
        base_text = best["routed"].read_text()
        tt, st = text_for(assign, cur_src, neck_power=False)
        trials = []
        rng = random.Random(len(names_open) * 7919 + len(ring))  # fixed seed: the same state gives the same trials
        for reach in REACHES:
            near = [i_ for i_ in ring if min(min((pos[i_] - pos[h]) % len(ring), (pos[h] - pos[i_]) % len(ring)) for h in hits) <= reach]
            order = []
            for i_ in near:
                nm = part.pads[i_].netname
                if nm and nm not in skip and "," not in nm and nm not in order:
                    order.append(nm)
            if not order:
                continue
            first = [n_ for n_ in order if n_ in names_open]
            orders = [("ring", order), ("open first", first + [n_ for n_ in order if n_ not in names_open]), ("ring reversed", order[::-1])]
            for sh in range(SHUFFLES):
                sq = order[:]
                rng.shuffle(sq)
                orders.append((f"shuffle {sh + 1}", sq))
            for tag, seq in orders:
                for vd in ((False, True) if a.package_via_drops else (False,)):  # the same order again with off-lattice pin-to-via drops (--via-drops): the designer's supply vias sit between the neighbours' tracks
                    if all(not (seq == t_[2] and vd == t_[3]) for t_ in trials):  # a small package: the wider reach adds nothing
                        trials.append((reach, tag + (" +drops" if vd else ""), seq, vd))
        if not trials:
            return False

        def one(ti: int, tr):
            """The group is ripped up and its nets routed one at a time, in the order of the trial, each on the copper of the ones before (--only-net: nothing
            already placed is ripped, so the order alone decides who gets which channel)."""
            reach, tag, seq, vd = tr
            stripped, nrem = pcbtext.strip_nets(base_text, {ids[n_] for n_ in seq if n_ in ids})
            wd = out / f"pkg-{k:02d}-{ti}"
            wd.mkdir(exist_ok=True)
            cur_t = stripped
            rpt = None
            for ni, n_ in enumerate(seq):
                rpt, ln = route(pcbtext.merge_copper(tt, cur_t), wd / f"n{ni:02d}", None, a.work, extra + (["--via-drops"] if vd else []) + ["--only-net", n_, "--pitch-um", "50", "--time", "40"], st, 1 + ti)
                cur_t = rpt.read_text()
            jj = judge(rpt, cur_src, wd)
            return {"reach": reach, "order": tag, "nets": len(seq), "removed": nrem, "opens": jj["opens"], "open": jj["open"], "violations": jj["violations"], "placement": jj["placement"], "routed": rpt}

        res_p = []
        with cf.ThreadPoolExecutor(TRIAL_JOBS) as ex:
            futs = [ex.submit(one, ti, tr) for ti, tr in enumerate(trials)]
            for f in cf.as_completed(futs):
                try:
                    res_p.append(f.result())
                except Exception as e:
                    log(f"package trial failed: {e}")
        key_ = lambda r_: (r_["reach"], r_["order"])
        rounds.append({"round": k, "kind": "package", "part": part.ref, "trials": [{x: r_[x] for x in ("reach", "order", "nets", "removed", "open", "violations")} for r_ in sorted(res_p, key=key_)]})
        log(f"{board[:24]} round {k}: package reroute of {part.ref} (open pins {len(hits)}): " + ", ".join(f"{r_['nets']} nets {r_['order']}: {r_['open']} open" for r_ in sorted(res_p, key=key_)))
        write_state()
        ok_p = [r_ for r_ in res_p if r_["open"] < best["open"] and r_["placement"] <= max(base_placement, best["placement"])]
        if not ok_p:
            return False
        w_ = min(ok_p, key=lambda r_: (r_["open"], r_["violations"], r_["nets"]))
        log(f"{board[:24]} package reroute of {part.ref} kept ({w_['nets']} nets, {w_['order']}): {best['open']} -> {w_['open']} open")
        best.update(open=w_["open"], violations=w_["violations"], placement=w_["placement"], routed=w_["routed"], opens=w_["opens"], dir=w_["routed"].parent, hard=set(HARD), assign=dict(assign))
        return True

    bundle_done: set = set()        # open-net sets the bundle reroute has been tried on

    def bundle_reroute() -> bool:
        """The router-level version of the designer's fix (doc 05 §50): every signal net with a pin on the dense package round an open pin is ripped up and the package's
        connections are placed again as bundles (`--bundle-escape`: parallel straight lanes in pin order, peeling off at their pads, placed before the search; the rest
        is routed round them), in a few option sets and seeds. Kept only if KiCad then shows fewer open connections."""
        if a.no_bundle or late() or any(r_.get("kind") == "bundle" and r_.get("round") == k for r_ in rounds):
            return False
        names_open = frozenset(n for n in j["opens"] if n)
        if not names_open or names_open in bundle_done:
            return False
        bq = kgeom.load(str(best["routed"]))
        ids = {nm: i_ for i_, nm in bq.nets.items()}
        pick = None
        for part in bq.parts:
            if len(part.pads) < 16:
                continue
            pitch_p = min((math.hypot(p1.x - p2.x, p1.y - p2.y) for i1, p1 in enumerate(part.pads) for p2 in part.pads[i1 + 1:] if math.hypot(p1.x - p2.x, p1.y - p2.y) > 0.05), default=9.0)
            if pitch_p > 0.65:
                continue
            hits = [i_ for i_, pd in enumerate(part.pads) if any(math.hypot(pd.x - x_, pd.y - y_) < 0.05 for pts in j["opens"].values() for (x_, y_) in pts)]
            if hits and (pick is None or len(hits) > len(pick[1])):
                pick = (part, hits)
        if pick is None:
            return False
        bundle_done.add(names_open)
        part = pick[0]
        skip = set(power) | set(HARD) | zone_nets
        nets = []
        for pd in part.pads:
            nm = pd.netname
            if nm and nm not in skip and "," not in nm and nm not in nets and nm in ids:
                nets.append(nm)
        base_text = best["routed"].read_text()
        tt, st = text_for(assign, cur_src, neck_power=False)
        stripped, nrem = pcbtext.strip_nets(base_text, {ids[n_] for n_ in nets})
        opts = [("bundle", []), ("bundle + corridors", ["--escape-plan", "--escape-length", "12"]), ("bundle + corridors + persist", ["--escape-plan", "--escape-length", "12", "--escape-persist"]),
                ("bundle + corridors, min 1", ["--escape-plan", "--escape-length", "12", "--bundle-min", "1"]),
                ("bundle + corridors + supply first", ["--escape-plan", "--escape-length", "12", "--bundle-min", "1", "--supply-first"]),
                ("bundle + corridors + supply first + tunnels", ["--escape-plan", "--escape-length", "12", "--bundle-min", "1", "--supply-first", "--package-tunnels"]),
                ("bundle + corridors + tunnels", ["--escape-plan", "--escape-length", "12", "--package-tunnels"])]
        trials = [(tag, ex, sd) for tag, ex in opts for sd in (0, 1, 2)]

        def one(ti: int, tr):
            tag, ex, sd = tr
            wd = out / f"bun-{k:02d}-{ti}"
            wd.mkdir(exist_ok=True)
            xtra = [x_ for x_ in extra if x_ != "--bundle-escape"] + ["--bundle-escape"] + ex + (["--seed", str(sd)] if sd else [])
            rpt, ln = route(pcbtext.merge_copper(tt, stripped), wd / "route", None, a.work, xtra, st, 1 + ti)
            jj = judge(rpt, cur_src, wd)
            return {"opt": tag, "seed": sd, "removed": nrem, "opens": jj["opens"], "open": jj["open"], "violations": jj["violations"], "placement": jj["placement"], "routed": rpt}

        res_b = []
        with cf.ThreadPoolExecutor(TRIAL_JOBS) as ex:
            futs = [ex.submit(one, ti, tr) for ti, tr in enumerate(trials)]
            for f in cf.as_completed(futs):
                try:
                    res_b.append(f.result())
                except Exception as e:
                    log(f"bundle trial failed: {e}")
        key_ = lambda r_: (r_["opt"], r_["seed"])
        rounds.append({"round": k, "kind": "bundle", "part": part.ref, "nets": len(nets), "trials": [{x: r_[x] for x in ("opt", "seed", "removed", "open", "violations")} for r_ in sorted(res_b, key=key_)]})
        log(f"{board[:24]} round {k}: bundle reroute of {part.ref} ({len(nets)} nets): " + ", ".join(f"{r_['opt']} s{r_['seed']}: {r_['open']} open" for r_ in sorted(res_b, key=key_)))
        write_state()
        ok_b = [r_ for r_ in res_b if r_["open"] < best["open"] and r_["placement"] <= max(base_placement, best["placement"])]
        if not ok_b:
            return False
        w_ = min(ok_b, key=lambda r_: (r_["open"], r_["violations"]))
        log(f"{board[:24]} bundle reroute of {part.ref} kept ({w_['opt']}, seed {w_['seed']}): {best['open']} -> {w_['open']} open")
        best.update(open=w_["open"], violations=w_["violations"], placement=w_["placement"], routed=w_["routed"], opens=w_["opens"], dir=w_["routed"].parent, hard=set(HARD), assign=dict(assign))
        return True

    def fine_finish() -> bool:
        """Narrow channels between fine-pitch pads are found only by a fine lattice (memsarray: pad 2 of a microphone, a 0.4 mm gap, routes at 10 um and not at 15)
        and only under the loosest tier for the net AND its neighbours (a gap is judged with the larger clearance of the two). Each open net in turn (up to 10), alone,
        on the copper of the best state, with `--only-net` and `--pitch-um`; kept only if KiCad then shows fewer open connections."""
        if a.no_fine or late() or any(r_.get("kind") == "fine" and r_.get("round") == k for r_ in rounds):
            return False
        names = [n for n in j["opens"] if n and "," not in n][:8]
        if not names or frozenset(names) in fine_done:
            return False
        fine_done.add(frozenset(names))
        bp = kgeom.load(str(best["routed"]))
        xs = [e[0] for e in bp.edge] + [e[2] for e in bp.edge]
        ys = [e[1] for e in bp.edge] + [e[3] for e in bp.edge]
        area = (max(xs) - min(xs)) * (max(ys) - min(ys)) if xs else 2500.0
        pitch_um = 10.0 if area <= 3600 else 14.0 if area <= 10000 else 20.0
        names = names[:8] if area <= 3600 else names[:4]  # a big board: fewer nets, the lattice is already coarser
        assign2 = dict(assign)
        cur = best["routed"]
        fd = out / f"fine-{k:02d}"
        fd.mkdir(exist_ok=True)
        steps = []
        for ni, n in enumerate(names):
            assign2[n] = max_idx
            for m in neighbours(kgeom.load(str(cur)), j["opens"][n], n):
                assign2[m] = max(assign2.get(m, 0), max_idx)
            tt, st = text_for(assign2, cur_src, neck_power=False)
            merged = pcbtext.merge_copper(tt, cur.read_text())
            wd = fd / f"net{ni}"
            rpt, ln = route(merged, wd, None, a.work, extra + ["--only-net", n, "--pitch-um", f"{pitch_um:g}", "--time", "150"], st, 1 + ni)  # a fine lattice needs many expansions: capped at 2.5 minutes a net (a 17-minute net stalled the batch)
            steps.append({"net": n, "router": ln[:60]})
            cur = rpt
        jj = judge(cur, cur_src, fd)
        rounds.append({"round": k, "kind": "fine", "pitch_um": pitch_um, "open": jj["open"], "violations": jj["violations"], "steps": steps})
        log(f"{board[:24]} round {k}: fine-lattice finish ({pitch_um:g} um) of {', '.join(n[:14] for n in names)}: {best['open']} -> {jj['open']} open")
        write_state()
        if jj["open"] < best["open"] and jj["placement"] <= max(base_placement, best["placement"]):
            assign.clear()
            assign.update(assign2)
            best.update(open=jj["open"], violations=jj["violations"], placement=jj["placement"], routed=cur, opens=jj["opens"], dir=fd, hard=set(HARD), assign=dict(assign))
            return True
        return False

    if a.resume:
        # the final state of an earlier run: its copper (routed.kicad_pcb: own rules and pours back, only the tracks and vias are used), tiers, hard pours and part moves
        rd = ROOT / "reports/realworld/boards" / board / a.resume
        c0 = json.loads((rd / "closure.json").read_text())
        assign.update({n: TIER_ORDER.index(t) for n, t in c0["assign"].items()})
        HARD.update(c0.get("hard", []))
        fine_done.update(frozenset(x) for x in c0.get("fine_done", []))
        for ref, (mdx, mdy) in c0["moves"].items():
            cur_src = pcbtext.move_part(cur_src, ref, mdx, mdy)
            moves[ref] = [mdx, mdy]
        text0, _ = text_for(assign, cur_src)
        j0 = judge(rd / "routed.kicad_pcb", cur_src, out)
        best = {"open": j0["open"], "violations": j0["violations"], "placement": j0["placement"], "routed": rd / "routed.kicad_pcb", "opens": j0["opens"], "src": cur_src,
                "dir": rd, "hard": set(HARD), "assign": dict(assign), "moves": {r_: list(v) for r_, v in moves.items()}}
        log(f"{board[:24]} resumed from {a.resume}: {best['open']} open")
    for k in range(0 if a.resume else a.rounds):
        if late():
            log(f"{board[:24]} deadline of {a.deadline_min:g} minutes passed: no further round")
            break
        shoved = None
        skip_raise: set[str] = set()   # nets the shove just put back under their tier: routed under it first, raised only if still open after that
        d = out / f"stage-{k:02d}-R"
        text, strict = text_for(assign, cur_src)
        rp, line = route(text, d, warm, a.work, extra + (a.round0.split() if k == 0 and warm is None and a.round0 else []), strict)
        j = judge(rp, cur_src, d)
        nrel = len(assign)
        labels.append(f"R{k}")
        rounds.append({"round": k, "kind": "route", "relaxed_nets": nrel, "relaxed_power": sum(1 for n in assign if n in power), "moved_parts": len(moves),
                       "router": line[:60], "open": j["open"], "violations": j["violations"], "placement_errors": j["placement"], "open_nets": sorted(j["opens"])})
        write_state()
        log(f"{board[:24]} round {k}: {j['open']} open ({len(j['opens'])} nets), {nrel} nets relaxed ({rounds[-1]['relaxed_power']} power), {len(moves)} parts moved; {line[:44]}")
        if best is None or j["open"] < best["open"] or (j["open"] == best["open"] and j["violations"] < best["violations"]):
            stall = 0 if best is None or j["open"] < best["open"] else stall + 1
            best = {"open": j["open"], "violations": j["violations"], "placement": j["placement"], "routed": rp, "opens": j["opens"], "src": cur_src, "dir": d, "hard": set(HARD), "assign": dict(assign), "moves": {r: list(v) for r, v in moves.items()}}
        else:
            stall += 1
        if j["open"] == 0:
            log("complete: KiCad shows no open connection")
            break
        if not a.no_shove and raised_last and shove_fails < 2:
            # Shove: the relaxation found a way through; now push the copper back to the board's rules. The nets that took a tier last time and are
            # connected now go one tier back; the router legalises the copper under those rules (vertices moved, long tracks cut so they can bend)
            # and keeps what is legal. Kept only if no connection opens and the board has fewer violations: then the compromise was never needed.
            resets = {n: assign[n] - 1 for n in raised_last if n in assign and n not in j["opens"]}
            if resets:
                assign2 = {n: v for n, v in assign.items() if n not in resets}
                assign2.update({n: v for n, v in resets.items() if v > 0})
                sd = out / f"shove-{k:02d}"
                tt, st = text_for(assign2, cur_src)
                rps, lns = route(tt, sd, rp, a.shove_work, extra + ["--legalize", "--legalize-subdivide", str(a.shove_cut)], st)
                js = judge(rps, cur_src, sd)
                # The nets that stay connected go one tier back for good. A connection the legaliser could not keep legal opens again: its net keeps
                # the tier it had (the next round routes it again under it), so the shove never costs a relaxation it needed.
                lost = {n for n in resets if n in js["opens"]}
                for n in lost:
                    assign2[n] = assign[n]
                skip_raise = set(lost) if lost else skip_raise
                took = (js["violations"] < j["violations"] and len(lost) < len(resets) and
                        (js["open"] - j["open"] <= max(3, j["open"] // 5) or js["open"] <= j["open"]))  # a shove that throws away a fifth of the work is no shove
                rounds.append({"round": k, "kind": "shove", "nets": len(resets), "open": js["open"], "violations": js["violations"], "was_open": j["open"], "was_violations": j["violations"],
                               "kept_back": len(resets) - len(lost), "accepted": took, "router": lns[:60]})
                shove_fails = 0 if took else shove_fails + 1
                log(f"{board[:24]} round {k}: shove {len(resets)} nets one tier back: {j['open']} -> {js['open']} open, {j['violations']} -> {js['violations']} violations, {len(resets) - len(lost)} of {len(resets)} stay back: {'kept' if took else 'discarded'}")
                if took:
                    assign.clear()
                    assign.update(assign2)
                    rp, j = rps, js
                    d = sd
                    shoved = rps
                    if js["open"] < best["open"] or (js["open"] == best["open"] and js["violations"] < best["violations"]):
                        best = {"open": js["open"], "violations": js["violations"], "placement": js["placement"], "routed": rps, "opens": js["opens"], "src": cur_src, "dir": sd, "hard": set(HARD), "assign": dict(assign), "moves": {r_: list(v) for r_, v in moves.items()}}
                    if js["open"] == 0:
                        log("complete: KiCad shows no open connection (after the shove)")
                        write_state()
                        break
                write_state()
        warm = shoved or best["routed"]  # a round that opened more than the best so far is discarded (the relaxation it paid for stays); a kept shove goes on
        # --- remedy 1/2: relax the nets in the way
        b = kgeom.load(str(rp))
        if not a.no_hard:
            # Pour islands: an open net that has a pour is routed pad to pad from now on, with its pour out of the router's board (back for KiCad's judging).
            # The cheapest remedy: no rule is relaxed and nothing moves; it costs copper.
            new_hard = {n for n in j["opens"] if n in zone_nets and n not in HARD}
            if new_hard:
                HARD.update(new_hard)
                skip_raise |= new_hard
                rounds.append({"round": k, "kind": "hard", "nets": sorted(new_hard)})
                log(f"{board[:24]} round {k}: pours of {', '.join(sorted(new_hard)[:6])}{' ...' if len(new_hard) > 6 else ''} taken out of the router's board: those nets route pad to pad")
                write_state()
                warm = shoved or best["routed"]
                continue  # route again before anything else is tried
        if not a.no_escape and not escape_tried and j["opens"]:
            # Open nets that end at pins of a dense package (8+ pads, pitch <= 0.65 mm): the escape plan with corridors of 12 mm (router options of the EtherCAT review,
            # doc 05 §47) reserves straight escapes for the whole package. EtherCAT: 177 -> 184 of 207 routed at the board's rules; other boards neutral.
            bq = kgeom.load(str(rp))
            dense_hits = 0
            for part in bq.parts:
                if len(part.pads) < 8:
                    continue
                pitch_p = min((math.hypot(p1.x - p2.x, p1.y - p2.y) for i1, p1 in enumerate(part.pads) for p2 in part.pads[i1 + 1:] if math.hypot(p1.x - p2.x, p1.y - p2.y) > 0.05), default=9.0)
                if pitch_p > 0.65:
                    continue
                dense_hits += sum(1 for pts in j["opens"].values() for (x_, y_) in pts if any(math.hypot(pd.x - x_, pd.y - y_) < 0.05 for pd in part.pads))
            if dense_hits >= 3:
                # a trial, not a switch: one round with the escape plan, kept (and left on) only if KiCad then shows fewer open connections
                escape_tried = True
                ed = out / f"escape-{k:02d}"
                tt, st = text_for(assign, cur_src)
                rpe, lne = route(tt, ed, best["routed"], a.work, extra + ["--escape-plan", "--escape-length", "12"], st)
                je = judge(rpe, cur_src, ed)
                took_e = je["open"] < best["open"] or (je["open"] == best["open"] and je["violations"] < best["violations"])
                rounds.append({"round": k, "kind": "escape", "dense_hits": dense_hits, "open": je["open"], "violations": je["violations"], "accepted": took_e})
                log(f"{board[:24]} round {k}: {dense_hits} open end points at pins of dense packages: escape plan trial (12 mm corridors): {best['open']} -> {je['open']} open: {'kept' if took_e else 'discarded'}")
                write_state()
                if took_e:
                    escape_on = True
                    extra.extend(["--escape-plan", "--escape-length", "12"])
                    best.update(open=je["open"], violations=je["violations"], placement=je["placement"], routed=rpe, opens=je["opens"], dir=ed, hard=set(HARD), assign=dict(assign))
                    warm = rpe
                    if best["open"] == 0:
                        break
                    continue
        if not a.no_copper_graphics and not copper_on and copper_graphics and j["opens"]:
            # Net-less copper drawn as board graphics (E3: an EasyEDA import's pours as polygons on the copper layers) is fixed copper no track may touch, and the pins
            # inside it are enclosed (+VB at U7 and U6: "boxed in" in every run). The option gives each polygon the net of the pads it touches (never ignores it: that shorted nets).
            copper_on = True
            extra.append("--soft-copper-graphics")
            rounds.append({"round": k, "kind": "copper_graphics"})
            log(f"{board[:24]} round {k}: --soft-copper-graphics on (board copper polygons take the net of the pads they touch)")
            write_state()
            warm = shoved or best["routed"]
            continue
        if not a.no_mask and not mask_on and mask_graphics and j["opens"]:
            # Net-less mask openings drawn as footprint graphics (solder jumpers, a microphone's pad group) are hard keep-outs for the router; KiCad only flags what
            # lies in them. Taken off as a whole once nets stay open: kitspace_test-jig could not start a track at 9 pins without it.
            mask_on = True
            extra.append("--soft-mask-graphics")
            rounds.append({"round": k, "kind": "mask"})
            log(f"{board[:24]} round {k}: --soft-mask-graphics on (footprint mask openings no longer keep copper out)")
            write_state()
            warm = shoved or best["routed"]
            continue
        before = dict(assign)
        sig_open = [n for n in j["opens"] if n not in power and n and n not in skip_raise]
        pw_open = [n for n in j["opens"] if n in power and n not in skip_raise]

        raised_last = set()

        def raise_nets(names: list[str]) -> None:
            """One tier more for each net in `names` (judged from the state before this round, so raises do not cascade) and the signal nets crowding it."""
            for n in names:
                if before.get(n, 0) < max_idx:
                    level = before.get(n, 0) + 1
                    assign[n] = max(assign.get(n, 0), level)
                    raised_last.add(n)
                    for m in neighbours(b, j["opens"][n], n):
                        if m not in power and m not in names:
                            assign[m] = max(assign.get(m, 0), min(level, max_idx))
                            raised_last.add(m)
        raise_nets(sig_open)
        if assign == before:  # no signal net can give any more: now, and only now, the power nets
            raise_nets(pw_open)
        if assign != before and stall < 2:
            continue
        if a.no_moves:
            if assign == before:
                log("nothing left to relax; moves disabled")
                break
            continue
        # --- remedy 3: move a part away from an open end point
        def effort() -> bool:
            """The last resort before stopping: one round at five times the work. Kept only if it opens fewer."""
            if a.no_effort or late() or any(r_.get("kind") == "effort" for r_ in rounds):
                return False
            ed = out / f"effort-{k:02d}"
            ex_ = extra  # stitching vias into the planes made every board worse in the first trials (3 -> 27, 1 -> 25, 18 -> 25 open): not used
            tt, st = text_for(assign, cur_src)
            rpt, ln = route(tt, ed, best["routed"], a.work * 5, ex_, st)
            jj = judge(rpt, cur_src, ed)
            rounds.append({"round": k, "kind": "effort", "open": jj["open"], "placement_errors": jj["placement"], "router": ln[:60]})
            log(f"{board[:24]} effort round (5x work): {best['open']} -> {jj['open']} open")
            write_state()
            if jj["open"] < best["open"]:
                best.update(open=jj["open"], violations=jj["violations"], placement=jj["placement"], routed=rpt, opens=jj["opens"], dir=ed)
                return True
            return False
        if fine_finish():
            warm = best["routed"]
            if best["open"] == 0:
                break
            continue
        if window_reroute():
            warm = best["routed"]
            if best["open"] == 0:
                break
            continue
        if late():
            break
        cands = candidates(b, j["opens"])[:a.move_trials]
        log(f"{board[:24]} round {k}: moving a part (stall {stall}); candidates {', '.join(c[0] for c in cands)}")
        if not cands:
            log("no part to move")
            if effort():
                warm = best["routed"]
                continue
            break
        text0, strict0 = text_for(assign, cur_src)
        trials = [(c, s) for s in STEPS_MM for c in cands][:a.move_trials * len(STEPS_MM)]

        def trial(ci: int, c, step: float):
            ref, ux, uy, _ = c
            td = out / f"trial-{k:02d}-{ci:02d}"
            t = pcbtext.move_part(cur_src, ref, ux * step, uy * step)
            seeds = [(p.x, p.y, max(p.w, p.h) / 2 + 0.05, p.net) for part in b.parts if part.ref == ref for p in part.pads]
            w, _ = pcbtext.strip_touching(rp.read_text(), seeds)
            (td).mkdir(exist_ok=True)
            (td / "warm.kicad_pcb").write_text(w)
            tt, st = text_for(assign, t)
            rpt, ln = route(tt, td, td / "warm.kicad_pcb", a.move_work, extra, st, 1 + ci % 32)
            jj = judge(rpt, t, td)
            return {"ref": ref, "step": step, "dx": ux * step, "dy": uy * step, "opens": jj["opens"], "open": jj["open"], "violations": jj["violations"], "placement": jj["placement"], "src": t, "routed": rpt, "line": ln}

        res = []
        with cf.ThreadPoolExecutor(TRIAL_JOBS) as ex:
            futs = [ex.submit(trial, ci, c, s) for ci, (c, s) in enumerate(trials)]
            for f in cf.as_completed(futs):
                try:
                    res.append(f.result())
                except Exception as e:  # a trial that fails is no trial
                    log(f"trial failed: {e}")
        ok = [r for r in res if r["open"] < best["open"] and r["placement"] <= max(base_placement, best["placement"])]
        rounds.append({"round": k, "kind": "move", "trials": [{x: r[x] for x in ("ref", "step", "open", "placement")} for r in sorted(res, key=lambda r: (r["ref"], r["step"]))]})
        if not ok:
            log("no part move opens fewer connections")
            write_state()
            if effort():
                warm = best["routed"]
                if best["open"] == 0:
                    break
                continue
            log("stuck")
            break
        w = min(ok, key=lambda r: (r["open"], r["placement"], r["step"], r["ref"]))
        log(f"{board[:24]} moved {w['ref']} by ({w['dx']:+.2f}, {w['dy']:+.2f}) mm: {best['open']} -> {w['open']} open")
        cur_src = w["src"]
        mv = moves.setdefault(w["ref"], [0.0, 0.0])
        mv[0] += w["dx"]
        mv[1] += w["dy"]
        warm = w["routed"]
        best = {"open": w["open"], "violations": w["violations"], "placement": w["placement"], "routed": w["routed"], "opens": w.get("opens", {}), "src": cur_src, "dir": w["routed"].parent, "hard": set(HARD), "assign": dict(assign), "moves": {r: list(v) for r, v in moves.items()}}
        stall = 0
        rounds[-1]["accepted"] = {"ref": w["ref"], "dx": w["dx"], "dy": w["dy"], "open": w["open"]}
        write_state()
    if best is not None and best["open"] > 0 and not a.no_finish:
        # The finishing phase: whatever the rounds ended with, the best state gets the two local remedies once more (the round budget may have ended before they ran).
        assign.clear()
        assign.update(best["assign"])
        HARD.clear()
        HARD.update(best["hard"])
        cur_src = best["src"]
        k = len(rounds) + 100
        j = {"opens": best.get("opens", {}), "open": best["open"], "placement": best["placement"], "violations": best["violations"]}
        log(f"{board[:24]} finishing phase on the best state ({best['open']} open)")
        for finish_pass in range(a.finish_passes):  # until neither step gains any more
            gained = False
            for step in (combine, package_reroute, bundle_reroute, fine_finish, window_reroute):
                if best["open"] == 0:
                    break
                k = len(rounds) + 100
                window_cap[0] = 80 if finish_pass == 0 else 240 if finish_pass == 1 else 500
                if step():
                    gained = True
                    log(f"{board[:24]} finishing pass {finish_pass + 1}: {step.__name__} -> {best['open']} open")
                    j = {"opens": best.get("opens", {}), "open": best["open"], "placement": best["placement"], "violations": best["violations"]}
            if best["open"] == 0 or (not gained and finish_pass >= 2):  # a pass without gain only ends the phase once the window cap has reached its largest
                break
    # the best state is the result, on a copy with the board's own rules; the baseline is the unrouted board with the parts where they were moved to
    (out / "routed.kicad_pcb").write_text(pcbtext.add_zones(pcbtext.restore_rules(best["routed"].read_text(), best["src"]), best["src"], best["hard"]))
    (out / "baseline.kicad_pcb").write_text(best["src"])
    assign.clear()
    assign.update(best["assign"])
    HARD.clear()
    HARD.update(best["hard"])
    moves.clear()
    moves.update(best["moves"])
    write_state()
    log(f"{board[:24]} {a.name}: best state {best['open']} open, {len(assign)} nets relaxed, {len(moves)} parts moved")
    return 0 if best["open"] == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
