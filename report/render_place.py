#!/usr/bin/env python3
"""Render a placement recording (`tracemaker-place --record`) as video frames.

  build/report-venv/bin/python report/render_place.py place_events.jsonl unrouted.kicad_pcb outdir \
      [--frames 240 --width 930 --height 700 --hold 20]

Each recorded state (input, the stages of the winning candidate, its annealing snapshots, ECO moves, the result)
is a key frame; positions are interpolated linearly between consecutive key frames so the motion is visible
(rotations switch half-way). Drawn in the report's board style: pads gold, moved parts teal with a faint trail
back to where the designer had them, fixed parts grey, ratsnest (minimum spanning tree per net) cyan, with the
stage and the ratsnest length in the corner. frame_00000.png is the designer's placement.

With --speed S (timed mode) frame k shows the job at real time k*S/fps: the state of the candidate that was kept,
as it existed at that moment (annealing snapshots carry their own times; a long gap is bridged by moving over the
last two seconds before the next recorded state), and the job's phase at that moment (building a candidate,
routing one, ...) as the caption.
"""
import argparse
import importlib.util
import json
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("mf", ROOT / "report/make_figures.py")
mf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mf)
plt = mf.plt
from matplotlib.collections import LineCollection, PatchCollection  # noqa: E402
from matplotlib.patches import FancyBboxPatch, Polygon  # noqa: E402

STAGE_NAMES = {
    "input": "Designer's placement",
    "quadratic": "Global placement: quadratic optimum",
    "spreading": "Global placement: spreading",
    "rotations": "Choosing rotations",
    "legalised": "Legalisation: no overlaps",
    "annealing": "Annealing: shorter connections, fewer crossings, routability",
    "lns": "Window re-placement",
    "placed": "Placement candidate",
    "eco": "Router-guided move",
    "result": "Result",
}


def rot90(x, y, r):
    """KiCad rotation by r * 90 degrees (y down), as place::rot90; bit 4 of r (a part flipped to the other side, D48)
    mirrors y first."""
    if r & 4:
        y = -y
    return [(x, y), (y, -x), (-x, -y), (-y, x)][r & 3]


def mst_length(pts):
    pts = list(dict.fromkeys(pts))
    n = len(pts)
    if n < 2:
        return 0.0, []
    inn, best, frm = [False] * n, [math.inf] * n, [0] * n
    best[0] = 0.0
    total, segs = 0.0, []
    for _ in range(n):
        u = min((i for i in range(n) if not inn[i]), key=lambda i: best[i])
        inn[u] = True
        if u:
            total += best[u]
            segs.append((pts[frm[u]], pts[u]))
        for v in range(n):
            if not inn[v]:
                d = abs(pts[u][0] - pts[v][0]) + abs(pts[u][1] - pts[v][1])
                if d < best[v]:
                    best[v], frm[v] = d, u
    return total, segs


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("events")
    ap.add_argument("board")
    ap.add_argument("outdir")
    ap.add_argument("--frames", type=int, default=240, help="frames for the motion (excluding --hold)")
    ap.add_argument("--hold", type=int, default=20, help="extra frames holding the result")
    ap.add_argument("--width", type=int, default=930)
    ap.add_argument("--height", type=int, default=700)
    ap.add_argument("--bbox", help="x0,y0,x1,y1 in mm (default: board outline + pads)")
    ap.add_argument("--speed", type=float, help="timed mode: real seconds per video second")
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--no-caption", action="store_true")
    a = ap.parse_args()
    out = pathlib.Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    rows = [json.loads(line) for line in open(a.events)]
    head = next(r for r in rows if r["type"] == "place_header")
    keys = [r for r in rows if r["type"] == "place_frame"]
    phases = [r for r in rows if r["type"] == "place_phase"]
    parts = head["parts"]
    ref_index = {p["ref"]: i for i, p in enumerate(parts)}

    d = mf.board_json(pathlib.Path(a.board))
    pads = [p for p in d["pads"] if p.get("ref") in ref_index]
    edges = mf.edge_cuts(pathlib.Path(a.board))
    final = keys[-1]["pos"]
    moved = {i for i, p in enumerate(parts) if (final[i][0], final[i][1], final[i][2]) != (p["x0"], p["y0"], 0)}

    def pad_shapes(pos):
        """Pad polygons and centres (mm) for a placement `pos` = [[x, y, r], ...] in nm."""
        polys, centres = [], []
        for p in pads:
            i = ref_index[p["ref"]]
            pt = parts[i]
            x, y, r = pos[i]
            ox, oy = p["x"] - pt["x0"], p["y"] - pt["y0"]
            dx, dy = rot90(ox, oy, int(r))
            cx, cy = (x + dx) / 1e6, (y + dy) / 1e6
            a0 = p.get("angle", 0.0)
            q = dict(p, x=x + dx, y=y + dy, angle=(-a0 if int(r) & 4 else a0) + 90.0 * (int(r) & 3))
            polys.append((i, mf.pad_poly(q)))
            centres.append((i, p.get("net"), (cx, cy)))
        return polys, centres

    # View box: outline and every pad position the recording visits.
    if a.bbox:
        x0, y0, x1, y1 = map(float, a.bbox.split(","))
    else:
        xs = [x for s in edges for x, _ in s]
        ys = [y for s in edges for _, y in s]
        for k in (keys[0], keys[-1]):
            for _, _, (cx, cy) in pad_shapes(k["pos"])[1]:
                xs.append(cx)
                ys.append(cy)
        x0, x1, y0, y1 = min(xs) - 1.5, max(xs) + 1.5, min(ys) - 1.5, max(ys) + 1.5
    want = a.width / a.height
    if (x1 - x0) / (y1 - y0) < want:
        c, w = (x0 + x1) / 2, (y1 - y0) * want
        x0, x1 = c - w / 2, c + w / 2
    else:
        c, h = (y0 + y1) / 2, (x1 - x0) / want
        y0, y1 = c - h / 2, c + h / 2

    dpi = 100
    fig = plt.figure(figsize=(a.width / dpi, a.height / dpi), dpi=dpi)
    ax = fig.add_axes([0, 0, 1, 1])
    bg = "#101418"
    ax.set_facecolor(bg)
    fig.patch.set_facecolor(bg)
    for seg in edges:
        xs_, ys_ = zip(*seg)
        ax.plot(xs_, ys_, color="#e9d66b", lw=1.2, zorder=1)
    ax.set_xlim(x0, x1)
    ax.set_ylim(y1, y0)
    ax.set_axis_off()
    body_coll = PatchCollection([], zorder=2)
    ax.add_collection(body_coll)
    trail_coll = LineCollection([], colors="#3fb8a8", linewidths=1.0, linestyles=(0, (3, 3)), alpha=0.55, zorder=2)
    ax.add_collection(trail_coll)
    pad_coll = PatchCollection([], edgecolor="none", zorder=4)
    ax.add_collection(pad_coll)
    rats_coll = LineCollection([], colors="#4fd6ff", linewidths=0.9, alpha=0.85, zorder=5)
    ax.add_collection(rats_coll)
    ref_texts = {}
    for i in moved:
        ref_texts[i] = ax.text(0, 0, parts[i]["ref"], color="#e8fffb", fontsize=9, ha="center", va="center", zorder=6,
                               family="DejaVu Sans", weight="bold")
    stage_t = ax.text(0.015, 0.985, "", transform=ax.transAxes, color="white", fontsize=15, va="top", family="DejaVu Sans",
                      bbox=dict(boxstyle="round,pad=0.35", fc=bg, ec="none", alpha=0.85), zorder=10)
    stat_t = ax.text(0.015, 0.02, "", transform=ax.transAxes, color="#4fd6ff", fontsize=14, va="bottom", family="DejaVu Sans",
                     bbox=dict(boxstyle="round,pad=0.35", fc=bg, ec="none", alpha=0.85), zorder=10)
    start_len = None
    origin = {i: None for i in moved}

    def draw(pos, stage_text, n):
        nonlocal start_len
        polys, centres = pad_shapes(pos)
        by_part = {}
        for i, poly in polys:
            by_part.setdefault(i, []).extend(poly)
        bodies, fcs, ecs = [], [], []
        for i, pts in by_part.items():
            bx0, by0 = min(x for x, _ in pts) - 0.35, min(y for _, y in pts) - 0.35
            bx1, by1 = max(x for x, _ in pts) + 0.35, max(y for _, y in pts) + 0.35
            bodies.append(FancyBboxPatch((bx0, by0), bx1 - bx0, by1 - by0, boxstyle="round,pad=0,rounding_size=0.3"))
            fcs.append("#14524b" if i in moved else "#262b31")
            ecs.append("#3fb8a8" if i in moved else "#4a5058")
            if i in ref_texts:
                ref_texts[i].set_position(((bx0 + bx1) / 2, by0 - 0.55))
            if i in origin and origin[i] is None:
                origin[i] = ((bx0 + bx1) / 2, (by0 + by1) / 2)
            if i in origin:
                by_part[i] = ((bx0 + bx1) / 2, (by0 + by1) / 2)
        body_coll.set_paths(bodies)
        body_coll.set_facecolors(fcs)
        body_coll.set_edgecolors(ecs)
        body_coll.set_linewidths(1.0)
        trail_coll.set_segments([(origin[i], by_part[i]) for i in moved if origin[i] and by_part.get(i) and origin[i] != by_part[i]])
        pad_coll.set_paths([Polygon(poly, closed=True) for _, poly in polys])
        pad_coll.set_facecolors(["#d9c27a" if i in moved else "#b8a46a" for i, _ in polys])
        nets = {}
        for _, net, c in centres:
            if net:
                nets.setdefault(net, []).append(c)
        total, segs = 0.0, []
        for pts in nets.values():
            if len(pts) <= 400:
                t, s = mst_length(pts)
                total += t
                segs.extend(s)
        rats_coll.set_segments(segs)
        if start_len is None:
            start_len = total
        stage_t.set_text(stage_text)
        stage_t.set_visible(bool(stage_text))
        delta = (total / start_len - 1) * 100 if start_len else 0.0
        stat_t.set_text(f"Ratsnest {total:,.0f} mm  ({delta:+.1f}%)" if n else f"Ratsnest {total:,.0f} mm")
        fig.savefig(out / f"frame_{n:05d}.png", dpi=dpi, facecolor=bg)

    def label(k):
        st = k["stage"]
        if st == "eco":
            m = k["label"]
            inner = m[m.find("(") + 1:m.find(":")] if "(" in m else ""
            return f"Router-guided move: {inner}" if inner else STAGE_NAMES["eco"]
        return STAGE_NAMES.get(st, st)

    if a.speed:
        job = head.get("seconds", keys[-1].get("t", 0.0))
        keys.sort(key=lambda k: k.get("t", 0.0))
        n_frames = int(math.ceil(job / a.speed * a.fps)) + 1

        def caption(t):
            txt = "Designer's placement"
            for ph in phases:
                if ph["t"] <= t:
                    txt = ph["text"]
            txt = txt.replace("building candidate placement: ", "building candidate: ").replace("router check: ", "router check of: ")
            return txt[0].upper() + txt[1:]
        j = 0
        for n in range(n_frames):
            t = min(job, n * a.speed / a.fps)
            while j + 1 < len(keys) and keys[j + 1]["t"] <= t:
                j += 1
            pos = keys[j]["pos"]
            if j + 1 < len(keys):
                nxt = keys[j + 1]
                span = min(2.0, nxt["t"] - keys[j]["t"])
                if span > 0 and t > nxt["t"] - span:
                    u = (t - (nxt["t"] - span)) / span
                    pos = [[xa + (xb - xa) * u, ya + (yb - ya) * u, rb if u >= 0.5 else ra]
                           for (xa, ya, ra), (xb, yb, rb) in zip(pos, nxt["pos"])]
            draw(pos, "" if a.no_caption else caption(t), n)
        print(f"{n_frames} frames over {job:.1f} s at {a.speed:g}x, {len(keys)} recorded states, {len(phases)} phases")
        return

    # Frame budget: every key-frame transition gets an equal share; key frames with identical positions are skipped.
    seq = [keys[0]]
    for k in keys[1:]:
        if k["pos"] != seq[-1]["pos"] or k is keys[-1]:
            seq.append(k)
    steps = max(1, len(seq) - 1)
    per = max(1, a.frames // steps)
    n = 0
    draw(seq[0]["pos"], label(seq[0]), n)
    n += 1
    for s in range(steps):
        pa, pb = seq[s]["pos"], seq[s + 1]["pos"]
        lab = label(seq[s + 1])
        for f in range(1, per + 1):
            t = f / per
            pos = [[xa + (xb - xa) * t, ya + (yb - ya) * t, rb if t >= 0.5 else ra] for (xa, ya, ra), (xb, yb, rb) in zip(pa, pb)]
            draw(pos, lab, n)
            n += 1
    for _ in range(a.hold):
        draw(seq[-1]["pos"], f"Result: {len(moved)} parts moved", n)
        n += 1
    print(f"{n} frames from {len(keys)} recorded states ({len(seq)} distinct), {len(moved)} parts moved")


if __name__ == "__main__":
    main()
