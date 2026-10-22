# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds and runs the ngspice cases of the report from routed boards.

A case names the nets to extract, the drivers (pulse sources with an output resistance), the quiet drivers (a net held at a level through a
resistance), the loads (pin capacitance and resistance) and what to probe. The routed copper of those nets is extracted (extract.py), the
passives between extracted nets are taken from the board's values, the nets are coupled where they run side by side, and ngspice runs the transient.
DC cases (current sinks on power and LED nets) extract the same copper and run an operating point.
"""
from __future__ import annotations

import math
import pathlib
import re
import subprocess
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import extract
import kgeom

SCRATCH = pathlib.Path("/home/dingo/Programming/TraceMaker/reports/realworld/sim/work")


def parse_value(s: str) -> float | None:
    s = s.strip().replace("µ", "u")
    m = re.fullmatch(r"([0-9]*\.?[0-9]+)\s*([pnumkKMGR]?)(?:[FfHhΩ]|ohm)?", s)
    if not m:
        m2 = re.fullmatch(r"([0-9]+)([RrKkMmuUnNpP])([0-9]+)", s)  # 4R7 style
        if not m2:
            return None
        base = float(f"{m2.group(1)}.{m2.group(3)}")
        mult = {"R": 1, "r": 1, "K": 1e3, "k": 1e3, "M": 1e6, "m": 1e-3, "u": 1e-6, "U": 1e-6, "n": 1e-9, "N": 1e-9, "p": 1e-12, "P": 1e-12}[m2.group(2)]
        return base * mult
    mult = {"": 1, "R": 1, "p": 1e-12, "n": 1e-9, "u": 1e-6, "m": 1e-3, "k": 1e3, "K": 1e3, "M": 1e6, "G": 1e9}[m.group(2)]
    return float(m.group(1)) * mult


def pulse(v, tr, period, duty=0.5, delay=2e-9):
    on = period * duty - tr
    return f"PULSE(0 {v} {delay:.4g} {tr:.4g} {tr:.4g} {on:.6g} {period:.6g})"


def build(board: kgeom.Board, layers: int, case: dict, gap_floor: float = 0.0):
    """Return (netlist text, nl, pairs) for a transient case."""
    ids = {n: i for i, n in board.nets.items()}
    nl = extract.Netlist()
    nets = list(case["nets"])
    for n in nets:
        extract.build_net(board, ids[n], nl, layers)
    # neighbours: every other net with copper running side by side with a net of the case. Rails (GND, +...) are held by their decoupling (AC ground);
    # other signals are quiet, loaded 10 kohm at their pins
    nb = neighbours(board, {ids[n] for n in nets}, case.get("couple_gap", 0.6))
    neighbour_names = []
    for nid in sorted(nb):
        name = board.nets.get(nid, "")
        if not name or name in nets:
            continue
        extract.build_net(board, nid, nl, layers)
        neighbour_names.append(name)
    allnets = {ids[n] for n in nets} | {ids[n] for n in neighbour_names}
    pairs = extract.couple(board, nl, allnets, max_gap=case.get("couple_gap", 0.6), gap_floor=gap_floor)
    extract.emit_caps(nl)
    lines = list(nl.lines)
    for k, (key, node) in enumerate(sorted(nl.pin_nodes.items())):
        if key[2] in neighbour_names:
            if key[2] == "GND" or key[2].startswith("+"):
                lines.append(f"Rrail{k} {node} 0 0.05")
            else:
                lines.append(f"Rnb{k} {node} 0 10000")
    built = set(nets)
    # passives between pins of built nets (resistors, capacitors, inductors by value); the far side of a passive whose other net is not built goes to ground
    for part in board.parts:
        if not part.ref or part.ref[0] not in "RCL":
            continue
        val = parse_value(part.value)
        if val is None:
            continue
        pn = [p for p in part.pads if p.netname in built]
        if not pn:
            continue
        pad_nodes = []
        for p in part.pads:
            key = (part.ref, p.num, p.netname)
            pad_nodes.append((p, nl.pin_nodes.get(key)))
        if len(part.pads) != 2:
            continue
        (pa, na), (pb, nb) = pad_nodes
        kind = part.ref[0]
        tag = f"{kind}x_{part.ref}"
        ext = case.get("external", {})
        if na and nb:
            lines.append(f"{tag} {na} {nb} {val:.6g}" if kind != "C" else f"{tag} {na} {nb} {val:.6g}")
        else:
            a, other = (na, pb) if na else (nb, pa)
            # other side: external node; ground for GND / supply nets, source impedance for the rest
            if other.netname == "GND" or other.netname.startswith("+"):  # rails are AC ground
                lines.append(f"{tag} {a} 0 {val:.6g}")
            else:
                en = f"ext_{part.ref}"
                lines.append(f"{tag} {a} {en} {val:.6g}")
                lines.append(f"Rext_{part.ref} {en} 0 {ext.get(part.ref, case.get('default_source', 1000.0)):.6g}")
    # supplies: a net held at its level by an ideal source through a small resistance (decoupled rails), pins named by the case
    for sn, spec in case.get("supply", {}).items():
        pin = nl.pin_nodes.get((spec["ref"], spec["pad"], sn))
        if pin:
            lines.append(f"Vs_{len(lines)} {pin}_s 0 {spec['v']}")
            lines.append(f"Rs_{len(lines)} {pin}_s {pin} {spec.get('r', 0.01)}")
    # drivers and loads
    probes = []
    for k, d in enumerate(case.get("drivers", [])):
        pin = nl.pin_nodes[(d["ref"], d["pad"], d["net"])]
        lines.append(f"Vd{k} d{k} 0 {pulse(d['v'], d['tr'], d['period'], d.get('duty', 0.5))}")
        lines.append(f"Rd{k} d{k} {pin} {d['rs']}")
    for k, q in enumerate(case.get("quiet", [])):
        pin = nl.pin_nodes[(q["ref"], q["pad"], q["net"])]
        lines.append(f"Vq{k} q{k} 0 {q['v']}")
        lines.append(f"Rq{k} q{k} {pin} {q['rs']}")
    for k, ld in enumerate(case.get("loads", [])):
        pin = nl.pin_nodes[(ld["ref"], ld["pad"], ld["net"])]
        if ld.get("c"):
            lines.append(f"Cl{k} {pin} 0 {ld['c']}")
        if ld.get("r"):
            lines.append(f"Rl{k} {pin} 0 {ld['r']}")
    for pr in case.get("probes", []):
        probes.append((pr, nl.pin_nodes[(pr["ref"], pr["pad"], pr["net"])]))
    return lines, nl, pairs, probes


def run_tran(name: str, lines: list, probes: list, stop: float, step: float, tag: str):
    SCRATCH.mkdir(parents=True, exist_ok=True)
    cir = SCRATCH / f"{name}.{tag}.cir"
    out = SCRATCH / f"{name}.{tag}.dat"
    sig = " ".join(f"v({n})" for _, n in probes)
    cir.write_text("\n".join([f"* {name} {tag}"] + lines + [".options method=gear reltol=1e-4 abstol=1e-12 vntol=1e-7 cshunt=1e-18",
                                                       ".control", f"tran {step:.4g} {stop:.4g} 0 {step:.4g}", f"wrdata {out} {sig}", "quit", ".endc", ".end"]) + "\n")
    r = subprocess.run(["ngspice", "-b", str(cir)], capture_output=True, text=True)
    log = (SCRATCH / f"{name}.{tag}.log")
    log.write_text(r.stdout + r.stderr)
    return out


def analyse(out: pathlib.Path, probes: list, skip: float):
    import numpy as np
    data = np.loadtxt(out)
    res = []
    for i, (pr, node) in enumerate(probes):
        t, v = data[:, 2 * i], data[:, 2 * i + 1]
        m = t >= skip
        base = float(np.median(v[m])) if pr.get("baseline") is None else pr["baseline"]
        dev = v[m] - base
        res.append({"label": pr.get("label", f"{pr['ref']}.{pr['pad']}"), "net": pr["net"], "base": base, "pk": float(np.max(np.abs(dev))), "pk_pos": float(np.max(dev)), "pk_neg": float(np.min(dev)),
                    "rms": float(np.sqrt(np.mean(dev ** 2))), "t": t, "v": v})
    return res


def disconnected(nl: extract.Netlist, nets: list) -> dict:
    """Nets whose pins do not all lie in one connected piece of extracted copper (series R and L elements only): an extraction or routing problem."""
    parent: dict = {}

    def f(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for l in nl.lines:
        p = l.split()
        if p[0][0] in "RL" and not p[0].startswith("Rx") and not p[0].startswith("Lx") and not p[0].startswith("Rext") and not p[0].startswith("Rd") and not p[0].startswith("Rq") and not p[0].startswith("Rl") and not p[0].startswith("Rs_"):
            parent[f(p[1])] = f(p[2])
    out = {}
    for n in nets:
        comps: dict = {}
        for k, v in nl.pin_nodes.items():
            if k[2] == n:
                comps.setdefault(f(v), []).append(f"{k[0]}.{k[1]}")
        if len(comps) > 1:
            out[n] = list(comps.values())
    return out


def neighbours(board: kgeom.Board, netids: set, max_gap: float) -> set:
    """Nets other than `netids` with a track side by side with a track of `netids` (gap under max_gap, run over 0.3 mm)."""
    import geomx
    tr = board.tracks
    cell = 4.0
    grid: dict = {}
    for i, t in enumerate(tr):
        for gx in range(int(min(t.x1, t.x2) // cell) - 1, int(max(t.x1, t.x2) // cell) + 2):
            for gy in range(int(min(t.y1, t.y2) // cell) - 1, int(max(t.y1, t.y2) // cell) + 2):
                grid.setdefault((gx, gy, t.layer), []).append(i)
    out = set()
    for t in tr:
        if t.net not in netids:
            continue
        cand = set()
        for gx in range(int(min(t.x1, t.x2) // cell) - 1, int(max(t.x1, t.x2) // cell) + 2):
            for gy in range(int(min(t.y1, t.y2) // cell) - 1, int(max(t.y1, t.y2) // cell) + 2):
                cand.update(grid.get((gx, gy, t.layer), []))
        for j in cand:
            u = tr[j]
            if u.net in netids or u.net == 0:
                continue
            run, cd = geomx.parallel_run((t.x1, t.y1, t.x2, t.y2), (u.x1, u.y1, u.x2, u.y2), max_gap + (t.w + u.w) / 2)
            if run > 0.3 and cd is not None:
                out.add(u.net)
    return out
