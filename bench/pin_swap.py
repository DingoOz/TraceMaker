#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Pin-swap feasibility experiment with a free-assignment oracle (doc 18 C2, doc 05 §56, M20).

Swappable pin groups are found by construction rules, not from the design's intent, so the experiment measures what the FREEDOM to swap
would be worth (an upper bound), not whether a given swap is legal:

- connector signal pins: a footprint whose reference starts J, P, CN, CON or X; pads of one type and size whose net is a signal (not a
  supply or ground by name, nor a differential pair member), has other pads elsewhere and only this one on the connector. Stands for a connector wired to interchangeable
  GPIO.
- resistor arrays: reference RN or RP with 4, 8 or 16 pads; a resistor is the pad pair (i, n+1-i); resistors swap as units and each may be
  turned end for end.

The oracle assigns the group's nets to its pads by a minimum-cost assignment (Kuhn 1955, the Hungarian method; scipy's
linear_sum_assignment) of the distance from each pad to the nearest pad of the net outside the part. A minimum Euclidean bipartite
matching has no crossing pairs, so on one layer it untangles the group's ratsnest.

  bench/pin_swap.py scan --set pcbench-bd-fail [--all]           # groups and the oracle's saving per board
  bench/pin_swap.py make --boards B1 B2 ... --out DIR            # DIR/<board>/unrouted.kicad_pcb with the oracle's nets
  bench/pin_swap.py compare --base RUN --swap RUN [--name NAME]  # table of routed / vias / length / clean, both runs judged by KiCad

The swapped board is written by permuting the pads' own (net ...) nodes, so everything else round-trips byte for byte.
"""
from __future__ import annotations

import argparse
import json
import math
import os
import pathlib
import re
import subprocess
import sys

from scipy.optimize import linear_sum_assignment

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = pathlib.Path(os.environ.get("TM_BINARY", ROOT / "build/release/src/app/tracemaker"))
SUPPLY = re.compile(r"(?i)(gnd|vcc|vdd|vss|vee|vbat|vbus|vin\b|vout|pwr|power|earth|shield|^[+-]|\d+v\d*|^v\d|3v3)")
CONNECTOR = re.compile(r"^(J|P|CN|CON|X)\d+$")
RARRAY = re.compile(r"^(RN|RP)\d+$")
PAIR = re.compile(r"(?i)(_[PN]|[+-]|_D[PMN]|_D[+-])$")  # a differential pair's member keeps its pin (the pair is routed as one)


# ---------- s-expression spans (enough to find footprints, pads and their net nodes) ----------

def tokenize(s: str):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in " \t\r\n":
            i += 1
        elif c in "()":
            yield c, i, i + 1
            i += 1
        elif c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 2 if s[j] == "\\" else 1
            yield "a", i, j + 1
            i = j + 1
        else:
            j = i
            while j < n and s[j] not in " \t\r\n()":
                j += 1
            yield "a", i, j
            i = j


class Node:
    __slots__ = ("start", "end", "items")

    def __init__(self, start):
        self.start, self.end, self.items = start, -1, []  # items: Node or (start, end) of an atom


def parse(s: str) -> Node:
    stack = [Node(0)]
    for kind, a, b in tokenize(s):
        if kind == "(":
            nd = Node(a)
            stack[-1].items.append(nd)
            stack.append(nd)
        elif kind == ")":
            nd = stack.pop()
            nd.end = b
        else:
            stack[-1].items.append((a, b))
    return stack[0].items[0]


def atom(s: str, it) -> str | None:
    if isinstance(it, tuple):
        t = s[it[0]:it[1]]
        return t[1:-1] if t.startswith('"') else t
    return None


def head(s: str, nd: Node) -> str | None:
    return atom(s, nd.items[0]) if nd.items else None


def footprints(s: str, root: Node):
    """(reference, {pad number: [net node span, ...]}) for every footprint/module."""
    for fp in root.items:
        if not isinstance(fp, Node) or head(s, fp) not in ("footprint", "module"):
            continue
        ref, pads = None, {}
        for ch in fp.items:
            if not isinstance(ch, Node):
                continue
            h = head(s, ch)
            if h == "fp_text" and len(ch.items) > 2 and atom(s, ch.items[1]) == "reference":
                ref = atom(s, ch.items[2])
            elif h == "property" and len(ch.items) > 2 and atom(s, ch.items[1]) == "Reference":
                ref = atom(s, ch.items[2])
            elif h == "pad" and len(ch.items) > 1:
                num = atom(s, ch.items[1])
                for g in ch.items:
                    if isinstance(g, Node) and head(s, g) == "net":
                        pads.setdefault(num, []).append((g.start, g.end))
        if ref:
            yield ref, pads


# ---------- groups and the oracle ----------

def board_json(path: pathlib.Path, scratch: pathlib.Path) -> dict:
    j = scratch / (path.parent.name + ".truth.json")
    if not j.exists():
        subprocess.run([str(TM), "inspect", str(path), "--json", str(j)], check=True, capture_output=True)
    return json.loads(j.read_text())


def groups_of(d: dict) -> list[dict]:
    pads = d["pads"]
    by_net: dict[str, list[dict]] = {}
    for p in pads:
        if p["net"]:
            by_net.setdefault(p["net"], []).append(p)
    by_ref: dict[str, list[dict]] = {}
    for p in pads:
        by_ref.setdefault(p["ref"], []).append(p)

    def others(p):
        return [q for q in by_net.get(p["net"], []) if q["ref"] != p["ref"]]

    def dist(p, net):
        o = [q for q in by_net[net] if q["ref"] != p["ref"]]
        return min(math.hypot(q["x"] - p["x"], q["y"] - p["y"]) for q in o) / 1e6

    out = []
    for ref in sorted(by_ref):
        ps = by_ref[ref]
        nums = [p["num"] for p in ps]
        if len(set(nums)) != len(nums):
            continue  # numbers repeat: the edit could not tell the pads apart
        if CONNECTOR.match(ref):
            kinds: dict[tuple, list[dict]] = {}
            for p in ps:
                if not p["net"] or SUPPLY.search(p["net"]) or PAIR.search(p["net"]) or not others(p) or sum(q["ref"] == ref for q in by_net[p["net"]]) != 1:
                    continue
                kinds.setdefault((p["attr"], p["w"], p["h"], tuple(p["layers"])), []).append(p)
            for k, g in kinds.items():
                if len(g) < 3:
                    continue
                nets = [p["net"] for p in g]
                cost = [[dist(p, n) for n in nets] for p in g]  # cost[pad][net]
                rows, cols = linear_sum_assignment(cost)
                before = sum(cost[i][i] for i in range(len(g)))
                after = sum(cost[r][c] for r, c in zip(rows, cols))
                assign = {g[r]["num"]: nets[c] for r, c in zip(rows, cols)}
                out.append({"ref": ref, "kind": "connector", "pads": [p["num"] for p in g], "nets": nets, "assign": assign,
                            "before_mm": round(before, 3), "after_mm": round(after, 3),
                            "moved": sum(1 for p in g if assign[p["num"]] != p["net"])})
        elif RARRAY.match(ref) and len(ps) in (4, 8, 16):
            n = len(ps)
            num = {p["num"]: p for p in ps}
            if not all(str(i) in num for i in range(1, n + 1)):
                continue
            units = [(num[str(i)], num[str(n + 1 - i)]) for i in range(1, n // 2 + 1)]
            if any(not a["net"] or not b["net"] or not others(a) or not others(b) for a, b in units):
                continue
            unets = [(a["net"], b["net"]) for a, b in units]

            def ucost(slot, un):
                a, b = slot
                return min(dist(a, un[0]) + dist(b, un[1]), dist(a, un[1]) + dist(b, un[0]))

            cost = [[ucost(s, un) for un in unets] for s in units]
            rows, cols = linear_sum_assignment(cost)
            before = sum(dist(a, a["net"]) + dist(b, b["net"]) for a, b in units)
            assign = {}
            for r, c in zip(rows, cols):
                a, b = units[r]
                n0, n1 = unets[c]
                if dist(a, n0) + dist(b, n1) <= dist(a, n1) + dist(b, n0):
                    assign[a["num"]], assign[b["num"]] = n0, n1
                else:
                    assign[a["num"]], assign[b["num"]] = n1, n0
            after = sum(cost[r][c] for r, c in zip(rows, cols))
            out.append({"ref": ref, "kind": "resistor_array", "pads": [p["num"] for p in ps], "nets": [p["net"] for p in ps], "assign": assign,
                        "before_mm": round(before, 3), "after_mm": round(after, 3),
                        "moved": sum(1 for p in ps if assign[p["num"]] != p["net"])})
    return out


def write_swapped(src: pathlib.Path, dst: pathlib.Path, groups: list[dict]) -> int:
    """Copies the board with each group's pads given the oracle's nets (the pads' (net ...) nodes permuted). Returns pads changed."""
    s = src.read_text()
    root = parse(s)
    fps = {ref: pads for ref, pads in footprints(s, root)}
    edits = []
    for g in groups:
        pads = fps.get(g["ref"], {})
        node_of_net = {}
        for num, net in zip(g["pads"], g["nets"]):
            spans = pads.get(num, [])
            if len(spans) != 1:
                raise ValueError(f"{src}: {g['ref']}.{num}: {len(spans)} net nodes")
            node_of_net[net] = s[spans[0][0]:spans[0][1]]
        for num, net in g["assign"].items():
            a, b = pads[num][0]
            if s[a:b] != node_of_net[net]:
                edits.append((a, b, node_of_net[net]))
    for a, b, t in sorted(edits, reverse=True):
        s = s[:a] + t + s[b:]
    dst.parent.mkdir(parents=True, exist_ok=True)
    dst.write_text(s)
    for side in src.parent.glob("unrouted.kicad_p*"):
        if side.suffix != ".kicad_pcb":
            (dst.parent / side.name).write_bytes(side.read_bytes())
    return len(edits)


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    sc = sub.add_parser("scan")
    sc.add_argument("--set")
    sc.add_argument("--boards", nargs="*")
    sc.add_argument("--all", action="store_true", help="every PCBench board")
    sc.add_argument("--json")
    mk = sub.add_parser("make")
    mk.add_argument("--boards", nargs="+", required=True)
    mk.add_argument("--out", required=True)
    cp = sub.add_parser("compare")
    cp.add_argument("--base", required=True)
    cp.add_argument("--swap", required=True)
    cp.add_argument("--name", required=True)
    cp.add_argument("--groups", help="scan JSON, for the group sizes in the table")
    a = ap.parse_args()
    scratch = ROOT / "build/scratch/pinswap"
    scratch.mkdir(parents=True, exist_ok=True)

    if a.cmd == "scan":
        if a.all:
            boards = sorted(p.name for p in FIX.iterdir() if (p / "unrouted.kicad_pcb").exists())
        elif a.set:
            boards = [x.strip() for x in (ROOT / "bench/sets" / f"{a.set}.txt").read_text().splitlines() if x.strip() and not x.startswith("#")]
        else:
            boards = a.boards
        res = {}
        for b in boards:
            try:
                gs = groups_of(board_json(FIX / b / "unrouted.kicad_pcb", scratch))
            except Exception as e:  # noqa: BLE001 - a board the reader refuses is skipped and named
                print(f"{b}: skipped ({e})", file=sys.stderr)
                continue
            gs = [g for g in gs if g["moved"] > 0 and g["after_mm"] < g["before_mm"] - 0.01]
            if gs:
                res[b] = gs
                tot_b = sum(g["before_mm"] for g in gs)
                tot_a = sum(g["after_mm"] for g in gs)
                print(f"{b}: {len(gs)} groups, {sum(g['moved'] for g in gs)} pads change, ratsnest {tot_b:.1f} -> {tot_a:.1f} mm")
        if a.json:
            pathlib.Path(a.json).write_text(json.dumps(res, indent=1))
        return 0

    if a.cmd == "make":
        out = pathlib.Path(a.out)
        for b in a.boards:
            gs = [g for g in groups_of(board_json(FIX / b / "unrouted.kicad_pcb", scratch)) if g["moved"] > 0 and g["after_mm"] < g["before_mm"] - 0.01]
            n = write_swapped(FIX / b / "unrouted.kicad_pcb", out / b / "unrouted.kicad_pcb", gs)
            (out / b / "swaps.json").write_text(json.dumps(gs, indent=1))
            print(f"{b}: {n} pads changed")
        return 0

    # compare
    def rows(run):
        p = ROOT / "bench/results" / run / "boards.jsonl"
        return {r["board"]: r for r in map(json.loads, p.read_text().splitlines()) if r}

    base, swap = rows(a.base), rows(a.swap)
    groups = json.loads(pathlib.Path(a.groups).read_text()) if a.groups else {}
    lines = [f"# Pin-swap oracle ({a.name})", "", f"Runs `{a.base}` (as designed) and `{a.swap}` (the oracle's nets), same options and work; KiCad judges both.", "",
             "| Board | Groups | Pads changed | Ratsnest in groups (mm) | Routed | Vias | Length (mm) | Clean |", "|---|--:|--:|---|---|---|---|---|"]
    table = []
    for b in sorted(set(base) & set(swap)):
        x, y = base[b], swap[b]
        gs = groups.get(b, [])
        lx = (x.get("style") or {}).get("length_mm", 0)
        ly = (y.get("style") or {}).get("length_mm", 0)
        t = {"board": b, "groups": len(gs), "pads": sum(g["moved"] for g in gs),
             "rat_before": round(sum(g["before_mm"] for g in gs), 1), "rat_after": round(sum(g["after_mm"] for g in gs), 1),
             "routed": [x["routed"], y["routed"]], "connections": [x["connections"], y["connections"]], "vias": [x["vias"], y["vias"]],
             "length": [round(lx, 1), round(ly, 1)], "clean": [x.get("clean"), y.get("clean")]}
        table.append(t)
        lines.append(f"| {b} | {t['groups']} | {t['pads']} | {t['rat_before']} → {t['rat_after']} | {x['routed']}/{x['connections']} → {y['routed']}/{y['connections']} | "
                     f"{x['vias']} → {y['vias']} | {t['length'][0]} → {t['length'][1]} | {'yes' if x.get('clean') else 'no'} → {'yes' if y.get('clean') else 'no'} |")
    out = ROOT / "bench/results" / a.name
    out.mkdir(parents=True, exist_ok=True)
    (out / "pin_swap.json").write_text(json.dumps(table, indent=1))

    def med(v):
        v = sorted(v)
        return v[len(v) // 2] if len(v) % 2 else (v[len(v) // 2 - 1] + v[len(v) // 2]) / 2

    if table:
        ob = sum(t["connections"][0] - t["routed"][0] for t in table)
        os_ = sum(t["connections"][1] - t["routed"][1] for t in table)
        better = sum(1 for t in table if t["connections"][1] - t["routed"][1] < t["connections"][0] - t["routed"][0])
        worse = sum(1 for t in table if t["connections"][1] - t["routed"][1] > t["connections"][0] - t["routed"][0])
        lines += ["", f"Totals: open connections {ob} → {os_} ({better} boards fewer, {worse} more), vias {sum(t['vias'][0] for t in table)} → "
                  f"{sum(t['vias'][1] for t in table)}, length {sum(t['length'][0] for t in table):.0f} → {sum(t['length'][1] for t in table):.0f} mm."]
        lines += ["", f"{len(table)} boards. Median change: open connections "
                  f"{med([(t['connections'][1] - t['routed'][1]) - (t['connections'][0] - t['routed'][0]) for t in table]):+}, vias "
                  f"{med([t['vias'][1] - t['vias'][0] for t in table]):+}, length {med([t['length'][1] - t['length'][0] for t in table]):+.1f} mm; clean "
                  f"{sum(1 for t in table if t['clean'][0])} → {sum(1 for t in table if t['clean'][1])}."]
    (out / "pin_swap.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
