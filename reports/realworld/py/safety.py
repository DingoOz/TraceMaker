#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Safety simulation of a relaxed routing: the step that makes a rule relaxation acceptable. Board-independent (no hand-written cases).

  safety.py BOARD RUN [--pairs N] [--drop-pct P]      (run inside the report image: ./dk.sh python3 py/safety.py BOARD RUN)

Reads boards/BOARD/RUN/{routed.kicad_pcb,ledger.json} (the routed copper with the board's own rules, and the compromise ledger of the KiCad DRC) and:

 1. CROSSTALK, every compromised pair: each pair of tracks that runs side by side closer than the board's clearance is screened with the field-solver +
    ngspice sweep (sim/results/xtalk_sweep.json; both nets assumed active, 1.5 ns edges, 40 ohm), then the N worst pairs get a full ngspice transient
    on the extracted copper of the two nets and of everything running beside them, in both directions (A drives and B is quiet, then the reverse), as
    routed and with the gap restored to the board's clearance (the no-compromise reference). Result in % of the logic swing: the metric does not
    depend on the supply voltage because the circuit is linear.
 2. CURRENT, every net with a narrowed track: IPC-2221 capacity of the narrowest piece (external layer, 1 oz, 10 C rise); the power-like nets also get
    an ngspice DC operating point of the extracted copper with an assumed load per sink pin and the voltage drop at each.
 3. VIAS: the smallest via of the layout, its resistance and inductance (field-solver values), and what that inductance is at the assumed edge rate
    against the driver resistance.

Verdict PASS / WARN / FAIL per check and overall (written to sim/results/BOARD/safety.RUN.json). The drivers, loads, edge rate and currents are
ASSUMPTIONS (stated in the file); a datasheet figure replaces them where there is one. Rails and plane nets are AC ground (as in spice.py).
"""
from __future__ import annotations

import json
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import numpy as np

import extract
import kgeom
import rules
import spice

RW = pathlib.Path(__file__).resolve().parents[1]
SWEEP = json.loads((RW / "sim/results/xtalk_sweep.json").read_text())

# assumptions of the check (each one is written into the result file)
ASSUME = dict(
    edge_ns=1.5, driver_ohm=40.0, load_pF=5.0, frequency_MHz=10.0,
    xtalk_pass_pct=10.0, xtalk_fail_pct=25.0,          # of the logic swing; 25 % is about the margin a CMOS input has to VIL/VIH
    sink_mA=50.0, drop_pass_pct=3.0, drop_fail_pct=8.0,  # per sink pin of a power net; drop as % of the rail
    ipc_margin=2.0,                                    # IPC-2221 capacity must be this many times the assumed total current
    via_pass_pct=5.0, via_fail_pct=20.0,               # via impedance (L/edge time) as % of the driver resistance
)
POWER_RE = re.compile(r"^(\+|V(CC|DD|BUS|IN|B|BAT)|PWR)|^[0-9]+V[0-9]*$|^\+?[0-9]+V[0-9]*", re.I)
CONNECTOR = re.compile(r"^(J|P|CON|USB|X)\d", re.I)
ACTIVE = re.compile(r"^(U|IC|Q|M|T|MOD|MK)\d", re.I)


def sweep_noise(layers: int, run: float, gap: float) -> float:
    """Peak noise (mV, 3.3 V swing) from the field-solver + ngspice sweep for a pair of parallel tracks: run (mm) and gap (mm), interpolated."""
    kind = f"{4 if layers >= 4 else 2}L_digital_tr1.5ns"
    vals = []
    for ln in (10.0, 25.0, 50.0):
        d = SWEEP[f"{kind}_L{ln:.0f}mm"]
        vals.append(np.interp(np.log(max(gap, 0.09)), np.log(d["gap"]), d["pk_mV"]))
    return float(np.interp(min(run, 50.0), [0.0, 10.0, 25.0, 50.0], [0.0] + vals))


def rail_voltage(name: str) -> float:
    m = re.search(r"([0-9]+)[Vv]([0-9]*)", name)
    if m:
        return float(f"{m.group(1)}.{m.group(2) or 0}")
    return 3.3


def ipc2221_amps(width_mm: float, oz: float = 1.0, dT: float = 10.0) -> float:
    """IPC-2221 external layer: I = 0.048 dT^0.44 A^0.725, A in mil^2."""
    area = (width_mm / 0.0254) * (1.378 * oz)
    return 0.048 * dT ** 0.44 * area ** 0.725


def pins_of(board: kgeom.Board, name: str) -> list:
    seen, out = set(), []
    for p in board.parts:
        for pad in p.pads:
            if pad.netname == name and (p.ref, pad.num) not in seen:
                seen.add((p.ref, pad.num))
                out.append((p.ref, pad.num))
    out.sort(key=lambda rp: (0 if re.match(r"^(U|IC)\d", rp[0]) else 1, rp))
    return out


def worst(vals: list[str]) -> str:
    for v in ("FAIL", "WARN", "PASS"):
        if v in vals:
            return v
    return "PASS"


def seg_dist(px, py, x0, y0, x1, y1):
    dx, dy = x1 - x0, y1 - y0
    L2 = dx * dx + dy * dy
    u = 0.0 if L2 == 0 else max(0.0, min(1.0, ((px - x0) * dx + (py - y0) * dy) / L2))
    return math.hypot(px - (x0 + u * dx), py - (y0 + u * dy))


def near_segments(board, net_ids, layer, at, radius, cap=40):
    """The tracks of the given nets on a layer within `radius` mm of a point: what to light up where a compromised pair was found."""
    out = []
    for t in board.tracks:
        if t.net in net_ids and (layer is None or t.layer == layer) and seg_dist(at[0], at[1], t.x1, t.y1, t.x2, t.y2) <= radius:
            out.append([round(t.x1, 3), round(t.y1, 3), round(t.x2, 3), round(t.y2, 3), round(t.w, 3), t.layer])
            if len(out) >= cap:
                break
    return out


def sim_dir(board, layers, clearance, aggr, vic, label):
    """One direction of a pair: `aggr` switches, `vic` is held quiet; the peak noise on the victim's pins as routed and with the gap restored to the rule."""
    import zlib
    pa, pv = pins_of(board, aggr), pins_of(board, vic)
    if not pa or not pv or len(pa) + len(pv) < 3:
        return {"aggressor": aggr, "victim": vic, "skipped": "net with fewer than two pins"}
    d = dict(ref=pa[0][0], pad=pa[0][1], net=aggr, v=3.3, rs=ASSUME["driver_ohm"], tr=ASSUME["edge_ns"] * 1e-9, period=1e-6 / ASSUME["frequency_MHz"], duty=0.5)
    case = dict(nets=[aggr, vic], layers=layers, drivers=[d],
                quiet=[dict(ref=pv[0][0], pad=pv[0][1], net=vic, v=0.0, rs=ASSUME["driver_ohm"])],
                loads=[dict(ref=r_, pad=p_, net=aggr, c=ASSUME["load_pF"] * 1e-12) for r_, p_ in pa[1:]] + [dict(ref=r_, pad=p_, net=vic, c=ASSUME["load_pF"] * 1e-12) for r_, p_ in pv[1:]],
                probes=[dict(ref=r_, pad=p_, net=vic, label=f"{vic} at {r_}.{p_}", baseline=0.0) for r_, p_ in pv[1:]] or [dict(ref=pv[0][0], pad=pv[0][1], net=vic, label=vic, baseline=0.0)])
    out = {"aggressor": aggr, "victim": vic}
    key = zlib.crc32(f"{aggr}|{vic}".encode()) % 10**8
    for tag, floor in (("routed", 0.0), ("nominal", clearance)):
        try:
            lines, nl, pairs, probes = spice.build(board, layers, case, gap_floor=floor)
            bad = spice.disconnected(nl, [aggr, vic])
            if bad:  # a net of the pair is still open in this routing: its circuit floats and ngspice diverges (1e8 % noise): not a result, so not simulated
                out["disconnected"] = True
                out["skipped"] = "a net of this pair is not connected in this routing (open in KiCad): the circuit floats, so it is not simulated"
                break
            f = spice.run_tran(f"safety_{label}_{key}", lines, probes, 0.4e-6, 0.1e-9, tag)
            an = spice.analyse(f, probes, 0.15e-6)
            out[tag] = round(max(x["pk"] for x in an) / 3.3 * 100, 2)
            if out[tag] > 150:  # noise larger than the swing itself is not physics: the circuit is floating or singular (an open net the extraction still joined)
                for k_ in ("routed", "nominal"):
                    out.pop(k_, None)
                out["disconnected"] = True
                out["skipped"] = "the simulation diverged (noise above 150 % of the swing): a net of this pair is open or the circuit is singular, so it is not a result"
                break
            w = max(an, key=lambda x: x["pk"])  # the worst victim pin: its waveform (mV, downsampled) goes into the result for the page
            keep = np.linspace(0, len(w["t"]) - 1, 160).astype(int)
            out.setdefault("wave", {"t_ns": [round(float(w["t"][k]) * 1e9, 2) for k in keep]})[f"{tag}_mV"] = [round(float(w["v"][k] - w["base"]) * 1e3, 2) for k in keep]
            out["victim_pin"] = w["label"]
            out["disconnected"] = bool(bad)
        except Exception as e:  # a pin the extraction could not place
            out["error"] = f"{type(e).__name__}: {e}"
            break
    return out


def crosstalk(board, ledger, layers, rails, clearance, npairs, label, emit):
    from concurrent.futures import ThreadPoolExecutor, as_completed
    ids = {n: i for i, n in board.nets.items()}
    byname = {n: i for i, n in board.nets.items()}
    agg: dict = {}
    skipped = 0
    for p in ledger["near"]:
        if p["nets"] and len(p["nets"]) == 2 and p["nets"][0] != p["nets"][1]:
            if any(n in rails or not n for n in p["nets"]):
                skipped += 1
                continue  # a rail or plane next to a signal is the signal's reference, not an aggressor
            run = p["run"] if p["kind"] == "track-track" else 1.0  # track beside a pad: about one pad width
            k = tuple(sorted(p["nets"]))
            a = agg.setdefault(k, {"run": 0.0, "gap": 9.0, "n": 0, "recs": []})
            a["run"] += max(run, 0.0)
            a["gap"] = min(a["gap"], p["gap"])
            a["n"] += 1
            a["recs"].append({"at": p["at"], "layer": p["layer"], "run": round(max(run, 0.0), 2), "gap": p["gap"]})
    rows = []
    for (n1, n2), a in agg.items():
        if a["run"] < 0.05:
            continue
        bound = sweep_noise(layers, a["run"], a["gap"]) / 3300 * 100
        nominal = sweep_noise(layers, a["run"], max(a["gap"], clearance)) / 3300 * 100
        rows.append(dict(nets=[n1, n2], run_mm=round(a["run"], 2), gap_mm=round(a["gap"], 3), sections=a["n"], bound_pct=round(bound, 2), bound_nominal_pct=round(nominal, 2), recs=a["recs"][:12]))
    rows.sort(key=lambda r: -r["bound_pct"])
    top_rows = rows[:npairs]
    ct = {"verdict": None, "pairs_screened": len(rows), "pairs_skipped_next_to_rail": skipped, "bound_worst_pct": round(max((r["bound_pct"] for r in rows), default=0.0), 2),
          "sim_worst_pct": 0.0, "screen": rows[:40], "simulated": [], "progress": {"done": 0, "total": len(top_rows)}}
    emit(ct)
    pending: dict = {}
    jobs = [(k, aggr, vic) for k, r in enumerate(top_rows) for aggr, vic in ((r["nets"][0], r["nets"][1]), (r["nets"][1], r["nets"][0]))]
    with ThreadPoolExecutor(max_workers=8) as ex:
        futs = {ex.submit(sim_dir, board, layers, clearance, a_, v_, label): (k, a_) for k, a_, v_ in jobs}
        for f in as_completed(futs):
            k, a_ = futs[f]
            pending.setdefault(k, []).append(f.result())
            if len(pending[k]) == 2:  # both directions of the pair are in
                r = top_rows[k]
                dirs = sorted(pending[k], key=lambda d: d["aggressor"] != r["nets"][0])
                ok = [d for d in dirs if "routed" in d]
                ct["simulated"].append({"nets": r["nets"], "run_mm": r["run_mm"], "gap_mm": r["gap_mm"], "dirs": dirs,
                                        "worst_routed_pct": max((d["routed"] for d in ok), default=None), "worst_nominal_pct": max((d["nominal"] for d in ok), default=None)})
                ct["simulated"].sort(key=lambda s: -(s["worst_routed_pct"] or 0))
                ct["sim_worst_pct"] = max(ct["sim_worst_pct"], ct["simulated"][0]["worst_routed_pct"] or 0)
                ct["progress"]["done"] += 1
                emit(ct)
    ct["pairs_open"] = sum(1 for s_ in ct["simulated"] if any(d.get("disconnected") for d in s_["dirs"]))  # not simulated: a net of the pair is open
    top = max(ct["bound_worst_pct"], ct["sim_worst_pct"])
    ct["verdict"] = "PASS" if top < ASSUME["xtalk_pass_pct"] else "WARN" if top < ASSUME["xtalk_fail_pct"] else "FAIL"
    return ct


def make_flags(board, res):
    """What the simulation says is wrong, with where it is on the board: crosstalk pairs (red/orange/amber/green by noise), narrowed power copper over its current limit,
    power nets with a voltage drop above the limit. Each flag has tracks (segs) and/or points (pts) in board millimetres, for the page to draw."""
    byname = {n: i for i, n in board.nets.items()}
    sev_of = lambda p: "fail" if p >= ASSUME["xtalk_fail_pct"] else "warn" if p >= ASSUME["xtalk_pass_pct"] else "watch" if p >= 5.0 else "ok"
    flags = []
    ct = res.get("crosstalk") or {}
    simd = {tuple(s["nets"]): s for s in ct.get("simulated", [])}
    for r in ct.get("screen", []):
        s = simd.get(tuple(r["nets"]))
        pct = s["worst_routed_pct"] if s and s["worst_routed_pct"] is not None else r["bound_pct"]
        ids_ = {byname[n] for n in r["nets"] if n in byname}
        segs, pts = [], []
        for rec in r.get("recs", []):
            pts.append([round(rec["at"][0], 2), round(rec["at"][1], 2)])
            if sev_of(pct) != "ok":
                segs += near_segments(board, ids_, rec["layer"], rec["at"], max(1.0, rec["run"] / 2 + 0.5), cap=24)
        flags.append({"kind": "crosstalk", "sev": sev_of(pct), "nets": r["nets"], "value": pct, "unit": "% of swing", "source": "simulated" if s else "screening bound",
                      "text": f"{r['nets'][0]} \u2194 {r['nets'][1]}: {pct:.1f} % of the logic swing ({'ngspice transient' if s else 'sweep bound'}); {r['run_mm']} mm side by side at {r['gap_mm']} mm gap",
                      "segs": segs[:80], "pts": pts})
    cu = res.get("current") or {}
    for sct in cu.get("sections", []):
        sev = "fail" if sct["ipc_A"] < ASSUME["ipc_margin"] * sct["I"] else "watch" if sct["ipc_A"] < 4 * sct["I"] else None
        if sev:
            flags.append({"kind": "current", "sev": sev, "nets": [sct["net"]], "value": sct["I"], "unit": "A",
                          "text": f"{sct['net']}: {sct['I']:.2f} A through {sct['w']} mm copper; IPC-2221 limit {sct['ipc_A']:.2f} A",
                          "segs": [[sct["x0"], sct["y0"], sct["x1"], sct["y1"], sct["w"], sct["layer"]]], "pts": []})
    for d in cu.get("dc", []):
        if d.get("worst_drop_pct", 0) >= ASSUME["drop_pass_pct"] and d.get("at"):
            flags.append({"kind": "drop", "sev": "fail" if d["worst_drop_pct"] >= ASSUME["drop_fail_pct"] else "warn", "nets": [d["net"]], "value": d["worst_drop_pct"], "unit": "% of rail",
                          "text": f"{d['net']}: {d['worst_drop_mV']} mV ({d['worst_drop_pct']} % of {d['rail_V']} V) at the farthest sink", "segs": [], "pts": [d["at"]]})
    order = {"fail": 0, "warn": 1, "watch": 2, "ok": 3}
    flags.sort(key=lambda f: (order[f["sev"]], -f["value"]))
    return flags


def currents(board, ledger, layers, rails, label):
    ids = {n: i for i, n in board.nets.items()}
    narrow: dict = {}
    for a in ledger["added"]:
        if a["type"] == "track_width":
            # the net of the narrow TRACK: the item list of a violation also holds the pour or pad it was found next to, and those nets are not narrowed
            tracks = [m.group(1) for it in a.get("items", []) if it.startswith("Track") for m in re.finditer(r"\[([^\]]*)\]", it)]
            for n in (tracks or a["nets"]):
                if n:
                    narrow[n] = min(narrow.get(n, 9.0), a["actual"])
    rows, power = [], []
    for n, w in sorted(narrow.items()):
        cap = ipc2221_amps(w)
        pins = pins_of(board, n)
        is_power = bool(POWER_RE.match(n)) and len(pins) >= 2
        rows.append(dict(net=n, narrowest_mm=w, ipc_A=round(cap, 2), power_like=is_power, pins=len(pins)))
        if is_power:
            power.append(n)
    drops = []
    sections: list = []
    if power:
        nl = extract.Netlist()
        for n in power:
            extract.build_net(board, ids[n], nl, layers)
        lines = [l for l in nl.lines if l[0] in "RL"]
        probes = []
        # the copper sections of a power net that are narrowed (width at most the narrowest + 2 %), whose branch current the operating point gives
        names = {ids[n]: n for n in power}
        narrow_secs = [(names[s["net"]], s["R"], s["w"], s) for s in nl.sections if s["net"] in names and s["w"] <= narrow[names[s["net"]]] * 1.02 + 1e-9]
        padpos = {(p.ref, pd.num): (pd.x, pd.y) for p in board.parts for pd in p.pads}
        for k, n in enumerate(power):
            pins = pins_of(board, n)
            src = next((p for p in pins if CONNECTOR.match(p[0])), pins[0])
            node = nl.pin_nodes.get((src[0], src[1], n))
            if not node:
                continue
            # DC load only on active parts (ICs, transistors, modules): decoupling capacitors, resistors and connectors draw nothing from the rail at DC
            sinks = [p for p in pins if p != src and ACTIVE.match(p[0])] or [p for p in pins if p != src][:1]
            lines.append(f"Vsrc{k} {node} 0 {rail_voltage(n)}")
            for j, p in enumerate(sinks):
                sn = nl.pin_nodes.get((p[0], p[1], n))
                if sn:
                    lines.append(f"Isk{k}_{j} {sn} 0 {ASSUME['sink_mA'] * 1e-3}")
                    probes.append((n, p, sn, rail_voltage(n), len(sinks)))
        if probes:
            sc = spice.SCRATCH
            sc.mkdir(parents=True, exist_ok=True)
            cir, dat = sc / f"safety_dc_{label}.cir", sc / f"safety_dc_{label}.dat"
            dat2 = sc / f"safety_dci_{label}.dat"
            sig = " ".join(f"v({p[2]})" for p in probes)
            isig = " ".join(f"@{r.lower()}[i]" for _, r, _, _ in narrow_secs)
            ctl = [".control", "op", "wrdata " + str(dat) + " " + sig]
            if narrow_secs:
                ctl.append("wrdata " + str(dat2) + " " + isig)
            cir.write_text("\n".join(["* safety dc"] + lines + ctl + ["quit", ".endc", ".end"]) + "\n")
            import subprocess
            subprocess.run(["ngspice", "-b", str(cir)], capture_output=True)
            vals = np.loadtxt(dat).reshape(-1)[1::2]
            cur: dict = {}
            if narrow_secs:
                iv = np.loadtxt(dat2).reshape(-1)[1::2]
                for (n, r, w, s), i in zip(narrow_secs, iv):
                    c = cur.setdefault(n, {"w": w, "I": 0.0})
                    c["I"] = max(c["I"], abs(float(i)))
                    sections.append({"net": n, "w": round(w, 3), "I": round(abs(float(i)), 4), "ipc_A": round(ipc2221_amps(w), 3), "layer": s["layer"],
                                     "x0": round(s["x0"], 3), "y0": round(s["y0"], 3), "x1": round(s["x1"], 3), "y1": round(s["y1"], 3)})
            per: dict = {}
            for (n, p, sn, vr, ns), v in zip(probes, vals):
                d = per.setdefault(n, {"rail_V": vr, "sinks": ns, "worst_drop_mV": 0.0})
                if (vr - float(v)) * 1e3 >= d["worst_drop_mV"]:
                    d["at"] = [round(padpos[(p[0], p[1])][0], 2), round(padpos[(p[0], p[1])][1], 2)] if (p[0], p[1]) in padpos else None
                d["worst_drop_mV"] = max(d["worst_drop_mV"], (vr - float(v)) * 1e3)
            for n, d in per.items():
                d["worst_drop_pct"] = round(d["worst_drop_mV"] / (d["rail_V"] * 10), 2)
                d["worst_drop_mV"] = round(d["worst_drop_mV"], 2)
                d["assumed_total_A"] = round(ASSUME["sink_mA"] * 1e-3 * d["sinks"], 3)
                d["net"] = n
                c = cur.get(n)
                if c:  # the current the assumed loads put through the narrowed copper itself, against what that width carries
                    d["narrowed_section_A"] = round(c["I"], 3)
                    d["narrowed_section_ipc_A"] = round(ipc2221_amps(c["w"]), 2)
                drops.append(d)
    verdicts = []
    for r in rows:
        dd = next((d for d in drops if d["net"] == r["net"]), None)
        need = dd.get("narrowed_section_A", dd["assumed_total_A"]) if dd else 2 * ASSUME["sink_mA"] * 1e-3
        r["assumed_A"] = need
        r["ipc_ok"] = r["ipc_A"] >= ASSUME["ipc_margin"] * need
        verdicts.append("PASS" if r["ipc_ok"] else "FAIL")
    for d in drops:
        verdicts.append("PASS" if d["worst_drop_pct"] < ASSUME["drop_pass_pct"] else "WARN" if d["worst_drop_pct"] < ASSUME["drop_fail_pct"] else "FAIL")
    return {"verdict": worst(verdicts), "narrowed_nets": rows, "dc": drops, "sections": sections}


def vias(board, ledger):
    vs = [v for v in board.vias]
    if not vs:
        return {"verdict": "PASS", "note": "no vias"}
    small = min(vs, key=lambda v: (v.drill, v.size))
    r, l = extract.via_params(small.drill, small.size)
    z = l / (ASSUME["edge_ns"] * 1e-9)
    pct = z / ASSUME["driver_ohm"] * 100
    return {"verdict": "PASS" if pct < ASSUME["via_pass_pct"] else "WARN" if pct < ASSUME["via_fail_pct"] else "FAIL",
            "smallest_drill_mm": small.drill, "smallest_size_mm": small.size, "R_mohm": round(r * 1e3, 3), "L_nH": round(l * 1e9, 3), "Z_at_edge_ohm": round(z, 3), "pct_of_driver": round(pct, 2),
            "count": len(vs)}


def main() -> int:
    args = sys.argv[1:]
    name, run = args[0], args[1]
    npairs = int(args[args.index("--pairs") + 1]) if "--pairs" in args else 8
    base = RW / "boards" / name
    routed = base / run / "routed.kicad_pcb"
    ledger = json.loads((base / run / "ledger.json").read_text())
    board = kgeom.load(str(routed))
    text = routed.read_text()
    layers = len([l for l in board.layers if l.endswith(".Cu")]) or 2
    rails = {"GND", "GNDA", "GNDD", "GNDPWR", "AGND", "DGND"}
    for z in rules.zone_blocks((base / "planes.kicad_pcb").read_text()):
        m = re.search(r'\(net_name "?([^")\s]*)"?\)', z)
        if m and m.group(1):
            rails.add(m.group(1))
    rails |= {n for n in board.nets.values() if n.startswith("+") or n.upper() in ("GND",)}
    clearance = rules.own_tier(text)["clr"]
    label = f"{name[:10]}_{run}"
    res = {"board": name, "run": run, "layers": layers, "clearance_mm": clearance, "assumptions": ASSUME, "rails_as_ac_ground": sorted(rails),
           "unconnected_kicad": ledger.get("unconnected"), "compromises": len(ledger.get("added", []))}
    out = RW / "sim/results" / name
    out.mkdir(parents=True, exist_ok=True)
    target = out / f"safety.{run}.json"

    def save(status):
        res["status"] = status
        res["flags"] = make_flags(board, res)
        tmp = target.with_suffix(".tmp")
        tmp.write_text(json.dumps(res, indent=1))
        tmp.replace(target)  # the page reads this file while it is written: swap it in whole

    res["status"] = "running"
    res["vias"] = vias(board, ledger)
    res["current"] = currents(board, ledger, layers, rails, label)
    save("running")
    res["crosstalk"] = crosstalk(board, ledger, layers, rails, clearance, npairs, label, lambda ct: (res.__setitem__("crosstalk", ct), save("running")))
    res["verdict"] = worst([res["crosstalk"]["verdict"], res["current"]["verdict"], res["vias"]["verdict"]])
    save("done")
    c, i, v = res["crosstalk"], res["current"], res["vias"]
    print(f"{name[:26]} {run}: SAFETY {res['verdict']}  crosstalk {c['verdict']} (screen {c['pairs_screened']} pairs, bound {c['bound_worst_pct']} %, simulated worst {c['sim_worst_pct']} %)  "
          f"current {i['verdict']} ({len(i['narrowed_nets'])} narrowed nets, {len(i['dc'])} power nets)  vias {v['verdict']} ({v.get('pct_of_driver')} % of driver)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
