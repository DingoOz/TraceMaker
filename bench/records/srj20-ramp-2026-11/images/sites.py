#!/usr/bin/env python3
"""For every open connection of every still-open board's best result: where is its ball in the BGA, and are its four dog-bone
via sites (the diagonal centres between it and its neighbours) free of other nets' copper?

A site is blocked when another net's track or via comes closer to the site centre than via radius + clearance (+ half its width).
Writes sites.json and prints a table.
"""
import json, math, pathlib, re, sys

HERE = pathlib.Path(__file__).resolve().parent
FIX = pathlib.Path("/home/dingo/Programming/TraceMaker/bench/data/tscircuit/boards/srj20")
SEG = re.compile(r"\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer \"([\w.]+)\"\)\s+\(net \"([^\"]+)\"\)")
VIA = re.compile(r"\(via\s+\(at ([-\d.]+) ([-\d.]+)\)\s+\(size ([\d.]+)\).*?\(net \"([^\"]+)\"\)", re.S)
VIA_R, CLR = 0.15, 0.1


def seg_dist(p, a, b):
    ax, ay = a; bx, by = b; px, py = p
    dx, dy = bx - ax, by - ay
    L = dx * dx + dy * dy
    t = 0 if L == 0 else max(0, min(1, ((px - ax) * dx + (py - ay) * dy) / L))
    return math.hypot(px - ax - t * dx, py - ay - t * dy)


def bga(b):
    """Ball positions (board mm) by pad number, net per pad, pitch, and the footprint origin. Assumes no rotation (checked)."""
    txt = (FIX / b / "unrouted.kicad_pcb").read_text()
    i = txt.index('(footprint "')
    blocks = txt.split("\t(footprint ")[1:]
    for blk in blocks:
        if "bga" not in blk.split("\n", 1)[0].lower() and '"bga_component"' not in blk:
            continue
        at = re.search(r"\(at ([-\d.]+) ([-\d.]+)( [-\d.]+)?\)", blk)
        ox, oy = float(at[1]), float(at[2])
        assert not at[3] or float(at[3]) == 0, "rotated BGA"
        pads = {}
        for m in re.finditer(r'\(pad "(\d+)" smd \w+\s+\(at ([-\d.]+) ([-\d.]+)\)\s+\(size ([\d.]+) [\d.]+\)\s+\(layers[^)]*\)\s*(\(net "([^"]+)"\))?', blk):
            pads[m[1]] = (ox + float(m[2]), oy + float(m[3]), m[6])
        if len(pads) > 50:
            return pads
    raise SystemExit(f"no BGA in {b}")


def main():
    index = json.load(open(HERE / "index.json"))
    out, rows = {}, []
    for b, info in index.items():
        pads = bga(b)
        xs = sorted({round(p[0], 3) for p in pads.values()})
        ys = sorted({round(p[1], 3) for p in pads.values()})
        pitch = round(min(x2 - x1 for x1, x2 in zip(xs, xs[1:])), 3)
        txt = open(info["pcb"]).read()
        segs = [((float(m[1]), float(m[2])), (float(m[3]), float(m[4])), float(m[5]), m[7]) for m in SEG.finditer(txt)]
        vias = [((float(m[1]), float(m[2])), float(m[3]) / 2, m[4]) for m in VIA.finditer(txt)]
        used = sum(1 for p in pads.values() if p[2])
        res = []
        for f in info["failures"]:
            net, ball = re.match(r"(\S+): bga_component\.(\d+)", f).groups()
            x, y, _ = pads[ball]
            ring = min(xs.index(round(x, 3)), len(xs) - 1 - xs.index(round(x, 3)), ys.index(round(y, 3)), len(ys) - 1 - ys.index(round(y, 3)))
            free = 0
            for sx in (-1, 1):
                for sy in (-1, 1):
                    s = (x + sx * pitch / 2, y + sy * pitch / 2)
                    blocked = any(n != net and seg_dist(s, a, c) < VIA_R + CLR + w / 2 - 1e-6 for a, c, w, n in segs) or \
                              any(n != net and math.dist(s, p) < VIA_R + CLR + r - 1e-6 for p, r, n in vias)
                    free += not blocked
            res.append({"net": net, "ball": ball, "ring": ring, "free_sites": free})
            rows.append((b, net, ball, ring, free))
        out[b] = {"pitch": pitch, "grid": [len(xs), len(ys)], "balls_with_net": used, "balls": len(pads), "open": res}
    json.dump(out, open(HERE / "sites.json", "w"), indent=1)
    for b, v in out.items():
        rings = [o["ring"] for o in v["open"]]
        print(f"{b}: grid {v['grid']}, pitch {v['pitch']}, {v['balls_with_net']}/{v['balls']} balls used; open: rings {rings}, "
              f"free dog-bone sites {[o['free_sites'] for o in v['open']]}")


main()
