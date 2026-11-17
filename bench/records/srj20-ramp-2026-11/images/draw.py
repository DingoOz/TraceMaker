#!/usr/bin/env python3
"""Each still-open srj20 board's best result, whole and zoomed on the open connections (dashed magenta, ringed pads).

Uses the routing report's renderer (figs.py, rt.py: the viewer's look; F.Cu red, B.Cu blue). Writes <board>_full.png and
<board>_zoomN.png (open connections within 3 mm of each other share a zoom).
"""
import json, math, pathlib, sys

HERE = pathlib.Path(__file__).resolve().parent
REP = pathlib.Path("/home/dingo/Programming/TraceMaker/reports")
sys.path.insert(0, str(REP / "srjvideo"))
sys.path.insert(0, str(REP / "routing-report"))
sys.path.insert(0, str(HERE.parent / "srj20-again"))
sys.argv = sys.argv[:1]
import figs, again  # noqa: E402

figs.FIGS = HERE / "png"
figs.FIGS.mkdir(exist_ok=True)


def summary_of(pcb):
    p = pathlib.Path(pcb)
    return p.parent / "out.json" if p.name == "out.kicad_pcb" else p.with_name(p.name + ".route.json")


def groups(pairs, near=3e6):
    out = []
    for a, b in pairs:
        for g in out:
            if any(math.dist(a, q) < near or math.dist(b, q) < near for pr in g for q in pr):
                g.append((a, b))
                break
        else:
            out.append([(a, b)])
    return out


if __name__ == "__main__":
    index = {}
    for b in again.drive.BOARDS:
        if b in again.CLEAN:
            continue
        op, pcb, what = again.pool(b)[0]
        rec = HERE / "rec" / b / "rec.jsonl"
        clip = figs.static_clip(pcb, rec)
        pairs = figs.open_pairs(clip, summary_of(pcb))
        fails = json.load(open(summary_of(pcb)))["failures"]
        figs.render(clip, 1600, 1200, 1e9, f"{b}_full.png", marks=pairs, label=f"{b}: {op} open, best board ({what})", glow=0.0)
        zs = []
        for k, (pr, f) in enumerate(zip(pairs, fails)):
            for end, (x, y) in zip("ab", pr):  # a = the BGA ball, b = the test point
                m = 2.0e6
                box = [x - m, y - m, x + m, y + m]
                c2 = figs.zoom(figs.static_clip(pcb, rec), box)
                name = f"{b}_open{k}{end}.png"
                figs.render(c2, 900, 900, 1e9, name, marks=[pr], label=f"{f.split(':')[0]} ({'ball' if end == 'a' else 'test point'} end)",
                            glow=0.0, mark_r=14)
                zs.append({"png": name, "conn": f, "end": end, "at_mm": [round(x / 1e6, 2), round(y / 1e6, 2)]})
        index[b] = {"open": op, "best": what, "pcb": pcb, "failures": fails, "zooms": zs}
    json.dump(index, open(HERE / "index.json", "w"), indent=1)
