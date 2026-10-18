#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes the LaTeX tables of the report from the result files (ledgers, simulations, logs). Run on the host: gen_tex.py"""
import collections
import json
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import kgeom

ROOT = pathlib.Path("/home/dingo/Programming/TraceMaker")
RW = ROOT / "reports/realworld"
OUT = RW / "tex/gen"
OUT.mkdir(parents=True, exist_ok=True)
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
BOARDS = {
    "T": dict(name="kitspace_teensy-fx", layers=4, final=["A", "B"], dirs={"A": "ladder-A", "B": "ladder-B"}),
    "S": dict(name="Hardware_Playground_serial_gw_ATMEGA328P", layers=2, final=["A", "C"], dirs={"A": "ladder-A", "C": "ladder-C"}),
}


def esc(s: str) -> str:
    s = str(s)
    for a, b in (("\\", "\\textbackslash{}"), ("&", "\\&"), ("%", "\\%"), ("#", "\\#"), ("_", "\\_"), ("$", "\\$"), ("{", "\\{"), ("}", "\\}"), ("~", "\\textasciitilde{}"), ("^", "\\textasciicircum{}")):
        if a == "\\":
            s = s.replace(a, "\x00")
        else:
            s = s.replace(a, b)
    s = s.replace("\x00", "\\textbackslash{}")
    return s.replace(">=", "$\\geq$").replace(">", "$>$").replace("<", "$<$")


def tt(s):
    return "\\texttt{" + esc(s) + "}"


def write(name, text):
    (OUT / name).write_text(text)


def table(cols, rows, spec=None, caption=None, label=None, small=True, star=False):
    """A bare tabular (the report supplies the float, the caption and the label)."""
    spec = spec or ("l" + "r" * (len(cols) - 1))
    out = [f"\\begin{{tabular}}{{{spec}}}", "\\toprule", " & ".join(cols) + " \\\\", "\\midrule"]
    for r in rows:
        out.append(" & ".join(str(c) for c in r) + " \\\\")
    out += ["\\bottomrule", "\\end{tabular}"]
    return "\n".join(out) + "\n"


def route_line(d: pathlib.Path):
    f = d / "route.log"
    if not f.exists():
        return None
    for l in f.read_text().splitlines():
        m = re.match(r"routed (\d+)/(\d+) connections, (\d+) tracks, (\d+) vias, pitch ([0-9.]+) mm, (\d+) expansions, ([0-9.]+) s", l)
        if m:
            return dict(routed=int(m.group(1)), total=int(m.group(2)), tracks=int(m.group(3)), vias=int(m.group(4)), seconds=float(m.group(7)))
    return None


def kind_of(items):
    ks = []
    for it in items:
        s = it.lower()
        if s.startswith("track"):
            ks.append("track")
        elif s.startswith("via"):
            ks.append("via")
        elif "pad" in s:
            ks.append("pad")
        elif s.startswith("zone"):
            ks.append("zone")
        elif "edge.cuts" in s:
            ks.append("edge")
        else:
            ks.append("other")
    return "--".join(sorted(ks))


def stats(path):
    b = kgeom.load(str(path))
    w = collections.Counter(round(t.w, 3) for t in b.tracks)
    v = collections.Counter((round(x.size, 2), round(x.drill, 2)) for x in b.vias)
    return b, w, v


def main():
    for key, B in BOARDS.items():
        name, layers = B["name"], B["layers"]
        bd = RW / "boards" / name
        ref = kgeom.load(str(FIX / name / "unrouted.kicad_pcb"))
        # --- facts
        smd = sum(1 for p in ref.parts for pd in p.pads if pd.kind == "smd")
        tht = sum(1 for p in ref.parts for pd in p.pads if pd.kind == "thru_hole")
        x = [e[0] for e in ref.edge] + [e[2] for e in ref.edge]
        y = [e[1] for e in ref.edge] + [e[3] for e in ref.edge]
        plane = json.load(open(bd / "planes.drc.json"))
        fixd = json.load(open(FIX / name / "unrouted.drc.json"))
        write(f"facts_{key}.tex", table(["Item", "Value"], [
            ["Board outline", f"{max(x) - min(x):.1f} $\\times$ {max(y) - min(y):.1f}~mm, {layers} copper layers"],
            ["Footprints (on the back)", f"{len(ref.parts)} ({sum(1 for p in ref.parts if p.side == 'B')})"],
            ["Pads: SMD / through-hole", f"{smd} / {tht}"],
            ["Nets with two or more pins", sum(1 for n in {pd.netname for p in ref.parts for pd in p.pads if pd.netname} if sum(1 for p in ref.parts for pd in p.pads if pd.netname == n) > 1)],
            ["Connections to route (fixture / with the designer's planes)", f"{len(fixd['unconnected_items'])} / {len(plane['unconnected_items'])}"],
            ["Board rules", f"clearance {ref.rules.get('trace_clearance')}~mm, minimum track {ref.rules.get('trace_min')}~mm, via {ref.rules.get('via_size')}/{ref.rules.get('via_drill')}~mm (minimum {ref.rules.get('via_min_size')}/{ref.rules.get('via_min_drill')})"],
            ["Net-class track widths", ", ".join(f"{esc(k)} {v['trace_width']}~mm" for k, v in ref.classes.items())],
        ], spec="p{5.6cm}p{8.2cm}", caption=None, small=True))
        # --- inherited DRC of the input (counts by type), fixture vs planes
        def counts(j):
            return collections.Counter(v["type"] for v in j["violations"] if v["severity"] == "error" or True)
        c = counts(plane)
        rows = [[tt(t), n] for t, n in sorted(c.items(), key=lambda x: -x[1]) if t not in ("lib_footprint_issues", "lib_footprint_mismatch", "text_height", "text_thickness", "silk_over_copper", "silk_overlap", "silk_edge_clearance", "mirrored_text_on_front_layer", "nonmirrored_text_on_back_layer", "unconnected_items")]
        write(f"inherited_{key}.tex", table(["KiCad rule", "violations already in the unrouted board"], rows, spec="lr"))
        # --- baseline routing table
        rows = []
        for lab, dirs in (("Fixture (no planes), board's own rules, 10 M work", "T0-10000000"), ("Fixture, 50 M work", "T0-50000000"),
                          ("Designer's planes, 10 M work", "planes-T0-10000000-softzones"), ("Designer's planes, 50 M work", "planes-T0-50000000-softzones"),
                          ("Planes, signals off the inner layers, 50 M work", "planes-T0-50000000-softzones_notrackson_In1CuIn2Cu")):
            r = route_line(bd / dirs)
            if r:
                rows.append([lab, f"{r['routed']} / {r['total']}", f"{r['total'] - r['routed']}", r["vias"], f"{r['seconds']:.0f}"])
        write(f"baseline_{key}.tex", table(["Baseline run (TraceMaker, 8 variants, KiCad-judged below)", "routed", "open", "vias", "s"], rows, spec="lrrrr"))
        # --- the designer's layout and our layouts
        lay = {"human": bd / "human"}
        for k, d in B["dirs"].items():
            lay[k] = bd / d
        rowsH = []
        L = {}
        for k, d in lay.items():
            led = json.load(open(d / "ledger.json"))
            b, w, v = stats(d / "routed.kicad_pcb")
            tl = sum(math.hypot(t.x2 - t.x1, t.y2 - t.y1) for t in b.tracks)
            L[k] = dict(led=led, b=b, w=w, v=v, tl=tl)
        cols = ["", "Designer"] + [f"Ours {k}" for k in B["final"]]
        keys = ["human"] + B["final"]
        def rowf(label, f):
            return [label] + [f(L[k]) for k in keys]
        rows = [rowf("Tracks / vias", lambda z: f"{len(z['b'].tracks)} / {len(z['b'].vias)}"),
                rowf("Track length (mm)", lambda z: f"{z['tl']:.0f}"),
                rowf("Track widths used (mm: segments)", lambda z: esc(", ".join(f"{w}: {n}" for w, n in sorted(z["w"].items())))),
                rowf("Via sizes used (pad/drill mm: vias)", lambda z: esc(", ".join(f"{a}/{b}: {n}" for (a, b), n in sorted(z["v"].items())))),
                rowf("Unconnected items (KiCad)", lambda z: z["led"]["unconnected"]),
                rowf("Violations added by the routing", lambda z: len(z["led"]["added"]))]
        write(f"layouts_{key}.tex", table(cols, rows, spec="p{4.6cm}" + "p{3.1cm}" * len(keys)))
        # --- ledger by rule and class
        types = collections.OrderedDict()
        for k in keys:
            for a in L[k]["led"]["added"]:
                types.setdefault((a["type"], a["class"]), {}).setdefault(k, 0)
                types[(a["type"], a["class"])][k] += 1
        rows = []
        for (t, c2), d in sorted(types.items(), key=lambda x: (x[0][0], x[0][1])):
            rows.append([tt(t), esc(c2)] + [d.get(k, 0) for k in keys])
        write(f"ledger_{key}.tex", table(["KiCad rule", "fab class"] + cols[1:], rows, spec="llr" + "r" * len(keys)))
        # --- clearance violations by kind of pair and the smallest actual value
        kinds = {}
        for k in keys:
            for a in L[k]["led"]["added"]:
                if a["type"] == "clearance":
                    kk = kind_of(a["items"])
                    d = kinds.setdefault(kk, {}).setdefault(k, [])
                    d.append(a["actual"])
        rows = []
        for kk, d in sorted(kinds.items(), key=lambda x: -sum(len(v) for v in x[1].values())):
            rows.append([esc(kk)] + [(f"{len(d[k])} (min {min(d[k]):.3f})" if k in d else "0") for k in keys])
        write(f"clrkinds_{key}.tex", table(["pair"] + cols[1:], rows, spec="l" + "r" * len(keys)))
        # --- nets carrying compromises, final layouts
        for k in B["final"]:
            led = L[k]["led"]
            b = L[k]["b"]
            nominal = {}
            for cn, cv in b.classes.items():
                for n in cv["nets"]:
                    nominal[n] = cv.get("trace_width", 0.25)
            byn = collections.defaultdict(lambda: dict(clr=0, minclr=9.0, drill=0))
            for a in led["added"]:
                for n in a["nets"] or ["?"]:
                    d = byn[n]
                    if a["type"] == "clearance":
                        d["clr"] += 1
                        d["minclr"] = min(d["minclr"], a["actual"])
                    if a["type"] == "drill_out_of_range":
                        d["drill"] += 1
            ids = {n: i for i, n in b.nets.items()}
            rows = []
            for n, d in byn.items():
                narrow = sum(math.hypot(t.x2 - t.x1, t.y2 - t.y1) for t in b.tracks if t.net == ids.get(n, -1) and t.w < nominal.get(n, 0.25) - 1e-6)
                tot = sum(math.hypot(t.x2 - t.x1, t.y2 - t.y1) for t in b.tracks if t.net == ids.get(n, -1))
                rows.append((d["clr"] + d["drill"], n, d, narrow, tot))
            rows.sort(key=lambda r: -r[0])
            tex = [[tt(n), d["clr"], f"{d['minclr']:.3f}" if d["clr"] else "--", d["drill"], f"{narrow:.1f} of {tot:.1f}"] for _, n, d, narrow, tot in rows[:14]]
            write(f"nets_{key}_{k}.tex", table(["net", "clearance items", "smallest gap (mm)", "small-via items", "narrowed track (mm)"], tex, spec="lrrrr"))
        # --- crosstalk tables
        cases = sorted({p.name.split(".")[0] for p in (RW / "sim/results" / name).glob("*.json") if not p.name.startswith("dc")})
        for case in cases:
            labs = ["human"] + B["final"]
            extra = f"{B['final'][-1]}_gap0.2" if key == "T" else f"{B['final'][0]}_gap0.2"
            data = {}
            for l in labs + [extra]:
                f = RW / "sim/results" / name / f"{case}.{l}.json"
                if f.exists():
                    data[l] = {p["label"]: p for p in json.load(open(f))["probes"]}
            if not data:
                continue
            order = list(data["human"].keys())
            rows = []
            for lbl in order:
                row = [esc(lbl)]
                for l in labs + [extra]:
                    p = data.get(l, {}).get(lbl)
                    if p is None:
                        row.append("--")
                        continue
                    row.append(f"{p['pk'] * 1e3:.1f}")
                rows.append(row)
            heads = ["probe (mV)", "Designer"] + [f"Ours {l}" for l in B["final"]] + [f"Ours {extra.replace('_gap0.2', '')}, gap 0.2"]
            write(f"xtalk_{key}_{case}.tex", table(heads, rows, spec="p{5.6cm}rrrr"))
        # --- DC
        dc = {}
        for l in labs:
            f = RW / "sim/results" / name / f"dc.{l}.json"
            if f.exists():
                dc[l] = json.load(open(f))
        nets = sorted({n for d in dc.values() for n in d["nets"]}, key=lambda n: -max((d["nets"].get(n, {}).get("worst_drop_mV", 0)) for d in dc.values()))
        rows = []
        for n in nets[:10]:
            row = [tt(n)]
            for l in labs:
                d = dc.get(l, {}).get("nets", {}).get(n)
                row.append("--" if not d else f"{d['worst_drop_mV']:.2f}")
            d0 = next(iter(dc.values()))["nets"].get(n)
            ours = dc.get(B["final"][-1], {}).get("nets", {}).get(n)
            row.append(f"{ours['w_min_mm']:.3f} / {ours['IPC2221_Imax_A_at_wmin']}" if ours else "--")
            row.append(f"{d0['I_total_A'] * 1e3:.0f}" if d0 else "--")
            rows.append(row)
        write(f"dc_{key}.tex", table(["net", "Designer"] + [f"Ours {l}" for l in B["final"]] + [f"narrowest (mm) / IPC limit (A), ours {B['final'][-1]}", "load (mA)"], rows, spec="lrrrrr"))
        # --- return paths
        rp = {}
        for l, d in lay.items():
            f = d / "return_paths.json"
            if f.exists():
                rp[l] = json.load(open(f))
        names = []
        for l in rp.values():
            for n in l["nets"]:
                if n["net"] not in names:
                    names.append(n["net"])
        rows = []
        for n in names:
            row = [tt(n)]
            for l in keys:
                d = next((x for x in rp.get(l, {"nets": []})["nets"] if x["net"] == n), None)
                row.append("--" if d is None else f"{d['signal_mm']:.0f} / {len(d['layer_changes'])} / {d['detour_mm']:.1f}" + (" $\\ast$" if d["warnings"] else ""))
            rows.append(row)
        write(f"rp_{key}.tex", table(["net"] + cols[1:], rows, spec="lrr" + "r" * (len(keys) - 2) if len(keys) > 2 else "lrr"))
    print("tex fragments written to", OUT)


if __name__ == "__main__":
    main()
