# SPDX-License-Identifier: GPL-3.0-or-later
"""Board figures for the routing report, drawn from the recorded runs in runs/ (rt.py, the viewer's look).

  python3 figs.py            -> figs/*.png
"""
import json, math, pathlib, re, sys

import numpy as np
import cairocffi as cairo
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
RUNS, FIGS = HERE / "runs", HERE / "figs"
sys.path.insert(0, str(HERE.parent / "srjvideo"))
import rt  # noqa: E402

MAG = (1.0, 0.35, 0.95)
WHITE = (0.93, 0.95, 0.98)
SEG = re.compile(r"\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer \"?([\w.]+)\"?\)")
VIA = re.compile(r"\(via\s+\(at ([-\d.]+) ([-\d.]+)\)\s+\(size ([\d.]+)\)")


def static_clip(pcb, like):
    """The routed board file's copper (all of it, from t = 0) on the recording's board (pads, outline)."""
    c = rt.Clip(str(like))
    txt = open(pcb, encoding="utf-8").read()
    segs = SEG.findall(txt)
    mm = lambda v: float(v) * 1e6
    c.ax = np.array([mm(s[0]) for s in segs]); c.ay = np.array([mm(s[1]) for s in segs])
    c.bx = np.array([mm(s[2]) for s in segs]); c.by = np.array([mm(s[3]) for s in segs])
    c.w = np.array([mm(s[4]) for s in segs])
    names = [l for l in c.board["layers"]] if isinstance(c.board.get("layers"), list) else []
    c.layer = np.array([0 if s[5] == "F.Cu" else 1 for s in segs])
    n = len(segs)
    c.t_add = np.zeros(n); c.t_rem = np.full(n, 1e18); c.tid = np.arange(n)
    c.burst = np.arange(n); c.gstart = np.zeros(n); c.gend = np.ones(n)
    vias = VIA.findall(txt)
    c.vx = np.array([mm(v[0]) for v in vias]); c.vy = np.array([mm(v[1]) for v in vias]); c.vd = np.array([mm(v[2]) for v in vias])
    c.vt_add = np.zeros(len(vias)); c.vt_rem = np.full(len(vias), 1e18)
    c.rats, c.fails = [], []
    return c


def open_pairs(clip, summary):
    refs = [f["ref"] for f in clip.board["footprints"]]
    centre = {}
    for p in clip.board["pads"]:
        pts = p["pts"]
        centre[(refs[p["fp"]], p["num"])] = (sum(q[0] for q in pts) / len(pts), sum(q[1] for q in pts) / len(pts))
    out = []
    for s in json.load(open(summary)).get("failures", []):
        m = re.search(r": (\S+)\.(\S+) -> (\S+)\.(\S+)", s)
        if m and (m[1], m[2]) in centre and (m[3], m[4]) in centre:
            out.append((centre[(m[1], m[2])], centre[(m[3], m[4])]))
    return out


def render(clip, w, h, u, name, marks=(), heat=None, label=None, glow=0.0, zones=False, mark_r=None):
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, w, h)
    cr = cairo.Context(surf)
    clip.draw(cr, w, h, u, glow=glow, dust=False)
    s, ox, oy = clip.view(w, h)
    P = lambda p: (p[0] * s + ox, p[1] * s + oy)
    if zones:
        for z in clip.board.get("zones", []):
            for poly in z["polys"]:
                ring = poly[0] if isinstance(poly[0][0], list) else poly
                cr.move_to(*P(ring[0]))
                for q in ring[1:]:
                    cr.line_to(*P(q))
                cr.close_path()
            cr.set_source_rgba(0.35, 0.85, 0.45, 0.16)
            cr.fill_preserve()
            cr.set_source_rgba(0.35, 0.85, 0.45, 0.7)
            cr.set_line_width(1.5)
            cr.stroke()
    if heat is not None:
        cr.set_operator(cairo.OPERATOR_ADD)
        d = np.array(heat["data"], float).reshape(heat["h"], heat["w"])
        mx = max(d.max(), 1.0)
        for j in range(heat["h"]):
            for i in range(heat["w"]):
                v = d[j, i] / mx
                if v <= 0.02:
                    continue
                x0, y0 = P((heat["x0"] + i * heat["cell"], heat["y0"] + j * heat["cell"]))
                cr.set_source_rgba(1.0, 0.55 + 0.4 * v, 0.1, 0.55 * v)
                cr.rectangle(x0, y0, heat["cell"] * s + 0.5, heat["cell"] * s + 0.5)
                cr.fill()
        cr.set_operator(cairo.OPERATOR_OVER)
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    for a, b in marks:
        (ax, ay), (bx, by) = P(a), P(b)
        cr.set_source_rgba(*MAG, 0.95)
        cr.set_line_width(max(1.5, w / 900))
        cr.set_dash([6, 4])
        cr.move_to(ax, ay); cr.line_to(bx, by); cr.stroke()
        cr.set_dash([])
        for px, py in ((ax, ay), (bx, by)):
            cr.set_line_width(max(1.5, w / 700))
            cr.arc(px, py, mark_r or max(6, w / 180), 0, 2 * math.pi)
            cr.stroke()
    if label:
        cr.select_font_face("Lato", cairo.FONT_SLANT_NORMAL, cairo.FONT_WEIGHT_BOLD)
        cr.set_font_size(max(16, w / 45))
        cr.set_source_rgba(*WHITE, 0.95)
        cr.move_to(14, 14 + max(16, w / 45))
        cr.show_text(label)
    Image.fromarray(rt.to_array(surf)).save(FIGS / name)
    print("wrote", name)


def stages(b, fractions, w, h, prefix):
    """The winner's recording when it had first routed the given share of the connections (stats events)."""
    c = rt.Clip(str(RUNS / f"{b}.jsonl"))
    c.speed = 1.0
    st = [json.loads(l) for l in open(RUNS / f"{b}.jsonl") if '"stats"' in l]
    total = st[0]["total"]
    for k, f in enumerate(fractions):
        t = next((e["t"] for e in st if e["routed"] >= f * total), 0.0) if f > 0 else 0.0
        n = next((e["routed"] for e in st if e["t"] >= t), 0) if f > 0 else 0
        render(c, w, h, t, f"{prefix}_{k}.png", label=f"t = {t:.1f} s: {n} of {total} routed", glow=0.4)


def zoom(clip, box):
    clip.bbox = list(box)
    clip.view = lambda w, h, pad=0.0, c=clip: rt.Clip.view(c, w, h, 0.02)
    return clip


if __name__ == "__main__":
    FIGS.mkdir(exist_ok=True)
    # 1. Hangul-Clock: the winner's recording at four moments, then the board as written.
    stages("hangul", (0.0, 0.25, 0.75), 1200, 760, "hangul_stage")
    hf = static_clip(RUNS / "hangul.routed.kicad_pcb", RUNS / "hangul.jsonl")
    render(hf, 1200, 760, 99, "hangul_stage_3.png", label="as written: 209 / 209 routed")
    # 2. EtherCAT: final board with the open connections, and the search heat map.
    ef = static_clip(RUNS / "ethercat.routed.kicad_pcb", RUNS / "ethercat.jsonl")
    marks = open_pairs(ef, RUNS / "ethercat.json")
    render(ef, 1600, 1100, 99, "ethercat_open.png", marks=marks)
    heats = [json.loads(l) for l in open(RUNS / "ethercat.jsonl") if '"heatmap"' in l]
    if heats:
        big = max(heats, key=lambda e: sum(e["data"]))
        ef2 = static_clip(RUNS / "ethercat.routed.kicad_pcb", RUNS / "ethercat.jsonl")
        render(ef2, 1600, 1100, 99, "ethercat_heat.png", heat=big)
    xs = [p[0] for m in marks for p in m]; ys = [p[1] for m in marks for p in m]
    if xs:
        cx, cy = np.median(xs), np.median(ys)
        ez = zoom(static_clip(RUNS / "ethercat.routed.kicad_pcb", RUNS / "ethercat.jsonl"), (cx - 14e6, cy - 10e6, cx + 14e6, cy + 10e6))
        render(ez, 1400, 1000, 99, "ethercat_zoom.png", marks=marks, mark_r=14)
    # 3. plane_smd: default (fills hard) and --soft-zones.
    pf = static_clip(RUNS / "plane_smd.routed.kicad_pcb", RUNS / "plane_smd.jsonl")
    render(pf, 1100, 900, 99, "plane_default.png", marks=open_pairs(pf, RUNS / "plane_smd.json"), label="default: fills are walls", zones=True)
    if (RUNS / "plane_soft.json").exists():
        ps = static_clip(RUNS / "plane_soft.routed.kicad_pcb", RUNS / "plane_smd.jsonl")
        render(ps, 1100, 900, 99, "plane_soft.png", marks=open_pairs(ps, RUNS / "plane_soft.json"), label="--soft-zones", zones=True)
    # 4. Option effects on Hangul-Clock (the M23 video's runs at 3 M work): plain, --power-width 0.5, + --octilinear.
    m23 = HERE.parent / "m23video"
    box = (158e6, 103e6, 188e6, 133e6)
    for f, lab in (("plain", "default"), ("pw", "--power-width 0.5"), ("m23", "--power-width 0.5 --octilinear")):
        c = zoom(static_clip(m23 / f"{f}.kicad_pcb", m23 / "plain.jsonl"), box)
        render(c, 900, 900, 99, f"opt_{f}.png", label=lab)
