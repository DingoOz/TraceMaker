#!/usr/bin/env python3
"""M26 F1: for every open BGA ball of the eleven boards' best results, what takes each of its four dog-bone via sites.

A site is taken by another net's via (centre within via radius + clearance + its radius) or track (within via radius + clearance + half
its width). Each blocker is classed: a via that is its own ball's dog-bone (on one of that ball's diagonal sites), any other via inside
the BGA, a via outside it, or a track on F.Cu / B.Cu. The blocker's net is a BGA net or not. Also: does the escape plan ever run
(escape_corridors in out.json). Writes blockers.json, prints a table.
"""
import collections, json, math, pathlib, re, sys
HERE = pathlib.Path(__file__).resolve().parent
IMG = HERE.parents[1] / "srj20-open-images"
sys.path.insert(0, str(IMG)); sys.argv = sys.argv[:1]
import importlib.util
spec = importlib.util.spec_from_file_location("sites", IMG / "sites.py")
src = (IMG / "sites.py").read_text().replace("\nmain()\n", "\n")
sites = {"__file__": str(IMG / "sites.py")}; exec(compile(src, "sites.py", "exec"), sites)
SEG, VIA, VIA_R, CLR, seg_dist, bga = sites["SEG"], sites["VIA"], sites["VIA_R"], sites["CLR"], sites["seg_dist"], sites["bga"]


def main():
    index = json.load(open(IMG / "index.json"))
    tot, out = collections.Counter(), {}
    for b, info in index.items():
        pads = bga(b)
        xs = sorted({round(p[0], 3) for p in pads.values()}); ys = sorted({round(p[1], 3) for p in pads.values()})
        pitch = round(min(x2 - x1 for x1, x2 in zip(xs, xs[1:])), 3)
        x0, x1, y0, y1 = xs[0] - pitch / 2, xs[-1] + pitch / 2, ys[0] - pitch / 2, ys[-1] + pitch / 2
        bga_nets = {p[2] for p in pads.values() if p[2]}
        own_sites = collections.defaultdict(list)  # net -> its ball's 4 diagonal sites
        for x, y, n in pads.values():
            if n:
                own_sites[n] += [(x + sx * pitch / 2, y + sy * pitch / 2) for sx in (-1, 1) for sy in (-1, 1)]
        txt = open(info["pcb"]).read()
        segs = [((float(m[1]), float(m[2])), (float(m[3]), float(m[4])), float(m[5]), m[6], m[7]) for m in SEG.finditer(txt)]
        vias = [((float(m[1]), float(m[2])), float(m[3]) / 2, m[4]) for m in VIA.finditer(txt)]
        j = json.load(open(pathlib.Path(info["pcb"]).parent / "out.json")) if info["pcb"].endswith("out.kicad_pcb") else {}
        c = collections.Counter()
        for f in info["failures"]:
            net, ball = re.match(r"(\S+): bga_component\.(\d+)", f).groups()
            x, y, _ = pads[ball]
            for sx in (-1, 1):
                for sy in (-1, 1):
                    s = (x + sx * pitch / 2, y + sy * pitch / 2)
                    kinds = set()
                    for p, r, n in vias:
                        if n != net and math.dist(s, p) < VIA_R + CLR + r - 1e-6:
                            own = any(math.dist(p, q) < 0.05 for q in own_sites.get(n, []))
                            inside = x0 <= p[0] <= x1 and y0 <= p[1] <= y1
                            kinds.add("via, its own dog-bone" if own else "via, inside BGA" if inside else "via, outside BGA")
                    for a, e, w, layer, n in segs:
                        if n != net and seg_dist(s, a, e) < VIA_R + CLR + w / 2 - 1e-6:
                            kinds.add(f"track {layer}" + ("" if n in bga_nets else ", not a BGA net"))
                    c["sites"] += 1
                    if not kinds:
                        c["free"] += 1
                    for k in kinds:
                        c[k] += 1
        out[b] = {"escape_corridors": j.get("escape_corridors"), **c}
        tot.update(c)
    json.dump({"boards": out, "total": tot}, open(HERE / "blockers.json", "w"), indent=1)
    print("escape corridors in the best results:", {b: v["escape_corridors"] for b, v in out.items()})
    print(f"sites of open balls: {tot['sites']}, free {tot['free']}; a site can have several kinds of blocker:")
    for k, v in sorted(tot.items(), key=lambda kv: -kv[1]):
        if k not in ("sites", "free"):
            print(f"  {k}: {v} sites ({100 * v / tot['sites']:.0f} %)")


main()
