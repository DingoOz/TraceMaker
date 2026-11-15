# SPDX-License-Identifier: GPL-3.0-or-later
"""TraceMaker placement closure video (D190), 60 s at 1080p30, on mechkeys 58r (PCBench).

Everything on screen comes from real runs (README.md):
  1. The designer's placement routed at 10 M work: a replay of the recorded event stream (human.jsonl) drawn by ../srjvideo/rt.py in
     the viewer's look, then the 6 connections it leaves open.
  2. The closure loop's 11 trials as `tracemaker-place --mode closure` logged them (closure.placed.json), with the parts each trial
     changed outlined. Kept changes move the parts from the designer's pose to the closure pose (the board files' footprints).
  3. The closure placement routed at the same 10 M work (closure.jsonl): 361 of 361.
  4. Evidence: KiCad 10 DRC on both routed boards (*.drc.json) and the 4x-budget control (bench/records/m24-closure).

  python3 video.py frames [JOBS]   -> frames/f_0000.png ...
  python3 video.py still SECONDS   -> still.png
"""
import json, math, pathlib, re, sys

import numpy as np
import cairocffi as cairo
from PIL import Image

HERE = pathlib.Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "srjvideo"))
import rt  # noqa: E402

W, H, FPS = 1920, 1080, 30
DUR = 60.0
FRAMES = HERE / "frames"

T_TITLE = (0.0, 5.0)
T_A = (4.6, 17.6)        # designer's placement routed
REPLAY_A = (5.4, 11.6)
T_FAIL = 11.9            # the open connections marked
T_4X = 14.2              # "4x the budget still leaves 4"
T_B = (17.2, 39.6)       # the closure loop
TRIAL0, DT = 20.6, 1.45  # first trial, seconds per trial
T_C = (39.2, 48.6)       # closure placement routed
REPLAY_C = (39.9, 46.0)
T_D = (48.2, 57.6)       # evidence
T_END = (57.2, 61.0)
RAMP, TXT_GAP, SWAP = 0.8, 0.7, 1.0

WHITE = (0.93, 0.95, 0.98)
MUTED = (0.58, 0.65, 0.76)
DIM = (0.40, 0.45, 0.55)
AMBER = (1.0, 0.72, 0.28)
OPEN = (1.0, 0.35, 0.95)  # magenta: open connections; red is the top copper
GREEN = (0.40, 0.92, 0.55)
CYAN = (0.45, 0.9, 1.0)
BG = rt.BG

FULL = (60, 230, 1800, 560)       # scenes A and C: the whole board (x, y, w, h)
ZOOM = (50, 215, 1120, 690)       # scene B: the corner where the parts moved
ZBOX = (27e6, 44e6, 113e6, 96e6)  # nm

# The loop's trials, in order, from closure.placed.json: (arm, change shown, before, after, kept, parts outlined, note)
TRIALS = [
    ("shift", "D41 1.0 mm right", 6, 6, False, ["D41"]),
    ("rotate", "R3 turned 180°", 6, 5, True, ["R3"]),
    ("rotate", "C4 turned 180°", 5, 5, False, ["C4"]),
    ("swap", "C4 and C9 swapped", 5, 5, False, ["C4", "C9"]),
    ("far", "D38 3 mm right", 5, 5, False, ["D38"]),
    ("rotate", "C6 turned 180°", 5, 7, False, ["C6"]),
    ("window", "re-place 15 parts", 5, 9, False, []),
    ("replace", "whole board re-placed", 5, 15, False, ["*"]),
    ("window", "re-place 15 parts", 5, 2, True, ["@window"]),
    ("room", "push C5, C7 1 mm away", 2, 9, False, ["C5", "C7"]),
    ("shift", "PROGRAM 0.5 mm down", 2, 0, True, ["PROGRAM"]),
]
FORCED = 8  # index of the trial the stall rule made radical


def ease(x):
    x = min(max(x, 0.0), 1.0)
    return x * x * (3 - 2 * x)


def fade(t, a, b, ramp=0.4):
    return ease((t - a) / ramp) * ease((b - t) / ramp)


def text(cr, s, x, y, size, col=WHITE, alpha=1.0, bold=False, align="left", mono=False):
    if alpha <= 0:
        return
    cr.select_font_face("DejaVu Sans Mono" if mono else "Lato", cairo.FONT_SLANT_NORMAL,
                        cairo.FONT_WEIGHT_BOLD if bold else cairo.FONT_WEIGHT_NORMAL)
    cr.set_font_size(size)
    ext = cr.text_extents(s)
    if align == "center":
        x -= ext[4] / 2
    elif align == "right":
        x -= ext[4]
    cr.set_source_rgba(*col, alpha)
    cr.move_to(x, y)
    cr.show_text(s)


SEG = re.compile(r"\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer \"?([\w.]+)\"?\)"
                 r"(?:.|\n)*?\(net (\d+)")
VIA = re.compile(r"\(via\s+\(at ([-\d.]+) ([-\d.]+)\)\s+\(size ([\d.]+)\)")


def static_clip(pcb, like):
    """A clip whose copper is the routed board file's, all present from t = 0 (the file has the after-route passes the recording lacks)."""
    c = rt.Clip(str(like))
    txt = open(pcb, encoding="utf-8").read()
    segs = SEG.findall(txt)
    mm = lambda v: float(v) * 1e6
    c.ax = np.array([mm(s[0]) for s in segs]); c.ay = np.array([mm(s[1]) for s in segs])
    c.bx = np.array([mm(s[2]) for s in segs]); c.by = np.array([mm(s[3]) for s in segs])
    c.w = np.array([mm(s[4]) for s in segs])
    c.layer = np.array([0 if s[5] == "F.Cu" else 1 for s in segs])
    n = len(segs)
    c.t_add = np.zeros(n); c.t_rem = np.full(n, 1e18); c.tid = np.arange(n)
    c.burst = np.arange(n); c.gstart = np.zeros(n); c.gend = np.ones(n)
    vias = VIA.findall(txt)
    c.vx = np.array([mm(v[0]) for v in vias]); c.vy = np.array([mm(v[1]) for v in vias]); c.vd = np.array([mm(v[2]) for v in vias])
    c.vt_add = np.zeros(len(vias)); c.vt_rem = np.full(len(vias), 1e18)
    c.rats, c.fails = [], []
    return c


def rot(x, y, a):
    c, s = math.cos(a), math.sin(a)
    return x * c - y * s, x * s + y * c


class Data:
    def __init__(self):
        self.human = rt.Clip(str(HERE / "human.jsonl"))
        self.closure = rt.Clip(str(HERE / "closure.jsonl"))
        for c in (self.human, self.closure):
            c.speed = 1.0
            c.pace = np.sort(c.t_add[c.t_add < c.tmax - 0.05])
        self.human_final = static_clip(HERE / "human.rec.kicad_pcb", HERE / "human.jsonl")
        self.closure_final = static_clip(HERE / "closure.rec.kicad_pcb", HERE / "closure.jsonl")
        a, b = self.human.board, self.closure.board
        self.refs = [f["ref"] for f in a["footprints"]]
        self.fa, self.fb = a["footprints"], b["footprints"]
        idx = {r: i for i, r in enumerate(self.refs)}
        moved = [i for i, (f, g) in enumerate(zip(self.fa, self.fb)) if (f["x"], f["y"], f["angle"]) != (g["x"], g["y"], g["angle"])]
        # Which kept trial moves which part: R3's turn is trial 2, PROGRAM's shift trial 11, the rest moved in the window (trial 9).
        self.when = {}
        for i in moved:
            k = {"R3": 1, "PROGRAM": 10}.get(self.refs[i], 8)
            self.when[i] = TRIAL0 + k * DT + 0.25
        self.window = [self.refs[i] for i in moved if self.when[i] == TRIAL0 + 8 * DT + 0.25]
        # Pads in the footprint's frame (KiCad's angle turns clockwise on screen, y down): p = centre + rot(local, -angle).
        self.pads = []
        for p in a["pads"]:
            f = self.fa[p["fp"]]
            loc = [rot(q[0] - f["x"], q[1] - f["y"], math.radians(f["angle"])) for q in p["pts"]]
            self.pads.append((p, loc))
        self.idx = idx
        # The open connections of the designer's placement, from the route's JSON (ref.pad -> ref.pad).
        centre = {}
        for p in a["pads"]:
            pts = p["pts"]
            centre[(self.refs[p["fp"]], p["num"])] = (sum(q[0] for q in pts) / len(pts), sum(q[1] for q in pts) / len(pts))
        self.open = []
        for s in json.load(open(HERE / "human.json"))["failures"]:
            m = re.search(r": (\S+)\.(\S+) -> (\S+)\.(\S+)", s)
            self.open.append((centre[(m[1], m[2])], centre[(m[3], m[4])]))
        self.outline = a["outline"]

    def pose(self, i, t):
        f, g = self.fa[i], self.fb[i]
        if i not in self.when:
            return f["x"], f["y"], f["angle"]
        k = ease((t - self.when[i]) / 0.9)
        da = (g["angle"] - f["angle"] + 180) % 360 - 180
        return f["x"] + (g["x"] - f["x"]) * k, f["y"] + (g["y"] - f["y"]) * k, f["angle"] + da * k


def view(box, w, h, pad=0.0):
    x0, y0, x1, y1 = box
    s = min(w * (1 - 2 * pad) / (x1 - x0), h * (1 - 2 * pad) / (y1 - y0))
    return s, (w - s * (x1 - x0)) / 2 - s * x0, (h - s * (y1 - y0)) / 2 - s * y0


def board_panel(cr, clip, rect, u, alpha):
    x, y, w, h = rect
    sub = cairo.ImageSurface(cairo.FORMAT_RGB24, w, h)
    clip.draw(cairo.Context(sub), w, h, u, glow=0.55)
    cr.save()
    cr.set_source_surface(sub, x, y)
    cr.paint_with_alpha(alpha)
    cr.restore()


def replay(cr, clip, final, rect, t, span, k):
    """The recorded route paced by event count, cross-faded to the board as written at the end."""
    swap = span[1] - SWAP
    f = min(max((t - span[0]) / (swap - span[0]), 0.0), 1.0)
    x = ease((t - swap) / SWAP)
    if x < 1:
        tr = clip.pace[int(f * (len(clip.pace) - 1))] if t >= span[0] else 0.0
        board_panel(cr, clip, rect, tr, k)
    if x > 0:
        board_panel(cr, final, rect, 99.0, k * x)


def mark_open(cr, D, clip, rect, k, t0, t):
    """Rings and lines on the connections the route left open."""
    if k <= 0:
        return
    x, y, w, h = rect
    s, ox, oy = clip.view(w, h)
    P = lambda p: (x + p[0] * s + ox, y + p[1] * s + oy)
    pulse = 0.75 + 0.25 * math.sin((t - t0) * 4.0)
    cr.save()
    cr.set_line_cap(cairo.LINE_CAP_ROUND)
    for a, b in D.open:
        (ax, ay), (bx, by) = P(a), P(b)
        cr.set_source_rgba(*OPEN, 0.9 * k)
        cr.set_line_width(2.5)
        cr.set_dash([8, 6])
        cr.move_to(ax, ay)
        cr.line_to(bx, by)
        cr.stroke()
        cr.set_dash([])
        for px, py in ((ax, ay), (bx, by)):
            cr.set_source_rgba(*OPEN, k * pulse)
            cr.set_line_width(3)
            cr.arc(px, py, 13, 0, 2 * math.pi)
            cr.stroke()
    cr.restore()


def parts_panel(cr, D, t, k, active):
    """Scene B: the pads only, each footprint at its pose at time t, zoomed to where the parts moved."""
    x, y, w, h = ZOOM
    s, ox, oy = view(ZBOX, w, h)
    X = lambda v: x + v * s + ox
    Y = lambda v: y + v * s + oy
    cr.save()
    cr.rectangle(x, y, w, h)
    cr.clip()
    cr.set_source_rgba(*rt.SUB, k)
    cr.paint()
    poses = {}
    boxes = {}
    for p, loc in D.pads:
        i = p["fp"]
        if i not in poses:
            poses[i] = D.pose(i, t)
        cx, cy, ang = poses[i]
        pts = [(cx + q[0], cy + q[1]) for q in (rot(l[0], l[1], -math.radians(ang)) for l in loc)]
        bx = boxes.setdefault(i, [1e18, 1e18, -1e18, -1e18])
        r = p["r"] or 0
        for qx, qy in pts:
            bx[0], bx[1] = min(bx[0], qx - r), min(bx[1], qy - r)
            bx[2], bx[3] = max(bx[2], qx + r), max(bx[3], qy + r)
        top = 0 in p["layers"]
        col = rt.TOP if top else rt.BOT
        cr.set_source_rgba(*col, (0.95 if top else 0.6) * k)
        if p["shape"] == "circle" or len(pts) == 1:
            cr.arc(X(pts[0][0]), Y(pts[0][1]), max(1.0, r * s), 0, 2 * math.pi)
            cr.fill()
        else:
            cr.move_to(X(pts[0][0]), Y(pts[0][1]))
            for q in pts[1:]:
                cr.line_to(X(q[0]), Y(q[1]))
            cr.close_path()
            cr.fill_preserve()
            if r:
                cr.set_line_width(max(1.0, 2 * r * s))
                cr.stroke()
            else:
                cr.new_path()
    cr.set_source_rgba(*rt.OUTL, 0.9 * k)
    cr.set_line_width(1.5)
    for ring in D.outline:
        cr.move_to(X(ring[0][0]), Y(ring[0][1]))
        for q in ring[1:]:
            cr.line_to(X(q[0]), Y(q[1]))
        cr.stroke()
    # The active trial's parts: amber while tried, green when the change is kept.
    if active is not None:
        j, a = active
        arm, what, before, after, kept, parts = TRIALS[j]
        col = GREEN if kept else AMBER
        refs = D.window if parts == ["@window"] else parts
        cr.set_line_width(3)
        if refs == ["*"]:
            cr.set_source_rgba(*col, a * k)
            cr.rectangle(x + 4, y + 4, w - 8, h - 8)
            cr.stroke()
        for ref in refs:
            if ref == "*":
                continue
            b = boxes[D.idx[ref]]
            m = 0.6e6
            cr.set_source_rgba(*col, a * k)
            cr.rectangle(X(b[0] - m), Y(b[1] - m), (b[2] - b[0] + 2 * m) * s, (b[3] - b[1] + 2 * m) * s)
            cr.stroke()
    cr.restore()


def trial_list(cr, t, k):
    x0, y0, rh = 1200, 300, 50
    text(cr, "#", x0, y0 - 46, 22, MUTED, k)
    text(cr, "kind", x0 + 50, y0 - 46, 22, MUTED, k)
    text(cr, "change tried", x0 + 170, y0 - 46, 22, MUTED, k)
    text(cr, "open", x0 + 590, y0 - 46, 22, MUTED, k, align="right")
    for j, (arm, what, before, after, kept, parts) in enumerate(TRIALS):
        t0 = TRIAL0 + j * DT
        a = ease((t - t0) / 0.3) * k
        if a <= 0:
            break
        yy = y0 + j * rh
        now = t0 <= t < t0 + DT
        if now:
            cr.set_source_rgba(1, 1, 1, 0.07 * a)
            cr.rectangle(x0 - 12, yy - 34, 680, rh - 4)
            cr.fill()
        done = t >= t0 + 0.7  # the trial's route result shows after it "ran"
        col = GREEN if (kept and done) else WHITE if now else MUTED
        text(cr, str(j + 1), x0, yy, 26, col, a)
        text(cr, arm, x0 + 50, yy, 26, col, a, bold=kept and done)
        text(cr, what, x0 + 170, yy, 26, col, a)
        res = f"{before} → {after}" if done else f"{before} → …"
        text(cr, res, x0 + 590, yy, 26, (GREEN if kept else OPEN if after > before else MUTED) if done else col, a, align="right", bold=kept and done)
        if done:
            text(cr, "kept" if kept else "undone", x0 + 605, yy, 22, GREEN if kept else DIM, a)
    # the stall rule, under the list, from the forced trial on
    a = ease((t - (TRIAL0 + FORCED * DT)) / 0.4) * k
    yy = y0 + len(TRIALS) * rh + 18
    text(cr, "Trials 3–8 won nothing, so trial 9 had to be radical.", x0, yy, 24, AMBER, a)
    text(cr, "Each trial is a full route of the whole board.", x0, yy + 36, 24, MUTED, k * ease((t - TRIAL0) / 0.5))


def open_count(t):
    n = 6
    for j, tr in enumerate(TRIALS):
        if tr[4] and t >= TRIAL0 + j * DT + 0.7:
            n = tr[3]
    return n


def frame(D, t):
    surf = cairo.ImageSurface(cairo.FORMAT_RGB24, W, H)
    cr = cairo.Context(surf)
    cr.set_source_rgb(*BG)
    cr.paint()

    def kt(span):
        return fade(t, span[0] + TXT_GAP, span[1] - TXT_GAP, 0.45)

    # 1. title
    k = fade(t, -1, T_TITLE[1], 0.5)
    if k > 0:
        text(cr, "Placement closure", W / 2, H / 2 - 60, 96, WHITE, k, bold=True, align="center")
        text(cr, "When the router can't finish a board,", W / 2, H / 2 + 20, 40, MUTED, k, align="center")
        text(cr, "TraceMaker moves parts until it can.", W / 2, H / 2 + 75, 40, MUTED, k, align="center")
        text(cr, "new in tracemaker-place --mode closure", W / 2, H / 2 + 170, 30, CYAN, k, align="center", mono=True)

    # 2. the designer's placement, routed
    k = fade(t, *T_A, RAMP)
    if k > 0:
        replay(cr, D.human, D.human_final, FULL, t, REPLAY_A, k)
        text(cr, "mechkeys 58r keyboard: the designer's placement, routed", W / 2, 120, 40, WHITE, kt(T_A), bold=True, align="center")
        text(cr, "same router and work budget all through this video (10 M units)", W / 2, 172, 28, MUTED, kt(T_A), align="center")
        mk = fade(t, T_FAIL, T_A[1] + 1, 0.5) * k
        mark_open(cr, D, D.human_final, FULL, mk, T_FAIL, t)
        tk = min(fade(t, T_FAIL, T_A[1], 0.45), kt(T_A))
        text(cr, "355 of 361 connections routed:  6 left open (magenta)", W / 2, 885, 38, OPEN, tk, bold=True, align="center")
        tk = min(fade(t, T_4X, T_A[1], 0.45), kt(T_A))
        text(cr, "Four times the work budget still leaves 4 open.", W / 2, 950, 32, WHITE, tk, align="center")

    # 3. the closure loop
    k = fade(t, *T_B, RAMP)
    if k > 0:
        j = int((t - TRIAL0) // DT)
        active = None
        if 0 <= j < len(TRIALS):
            u = t - (TRIAL0 + j * DT)
            active = (j, fade(u, 0.0, DT, 0.2))
        parts_panel(cr, D, t, k, active)
        tk = kt(T_B)
        text(cr, "Try one change, route the whole board, keep it only if fewer connections stay open", W / 2, 95, 34, WHITE, tk,
             bold=True, align="center")
        text(cr, "A bandit learns which kind of change pays off: small moves first, radical ones after a stall", W / 2, 145, 28, CYAN,
             tk, align="center")
        trial_list(cr, t, tk)
        n = open_count(t)
        text(cr, "connections open:", ZOOM[0] + 10, 990, 34, MUTED, tk)
        text(cr, str(n), ZOOM[0] + 310, 1000, 64, GREEN if n == 0 else OPEN, tk, bold=True)
        text(cr, "parts only; the board's corner where the parts moved", ZOOM[0] + ZOOM[2], 940, 22, DIM, tk, align="right")

    # 4. the closure placement, routed
    k = fade(t, *T_C, RAMP)
    if k > 0:
        replay(cr, D.closure, D.closure_final, FULL, t, REPLAY_C, k)
        text(cr, "The closure placement, routed with the same budget", W / 2, 120, 40, WHITE, kt(T_C), bold=True, align="center")
        text(cr, "16 parts moved, 3 changes kept, 11 trials in 24 minutes", W / 2, 172, 28, MUTED, kt(T_C), align="center")
        tk = min(fade(t, REPLAY_C[1] - 0.3, T_C[1], 0.45), kt(T_C))
        text(cr, "361 of 361 connections routed:  100 %", W / 2, 885, 42, GREEN, tk, bold=True, align="center")

    # 5. evidence
    k = fade(t, *T_D, RAMP)
    if k > 0:
        tk = kt(T_D)
        text(cr, "Checked by KiCad 10's own DRC", W / 2, 140, 48, WHITE, tk, bold=True, align="center")
        cols = (180, 1090, 1390, 1700)
        hy = 270
        for c, s in zip(cols, ("mechkeys 58r", "routed", "KiCad unconnected", "DRC errors added")):
            text(cr, s, c, hy, 26, MUTED, tk, align="left" if c == cols[0] else "center")
        rows = (("Designer's placement, 10 M work", "355 / 361", "6", "0", OPEN),
                ("Designer's placement, 40 M work (4×)", "357 / 361", "4", "0", OPEN),
                ("After placement closure, 10 M work", "361 / 361", "0", "0", GREEN))
        for r, (name, routed, unc, added, col) in enumerate(rows):
            yy = hy + 80 + r * 72
            a = tk * ease((t - (T_D[0] + 1.0 + r * 0.6)) / 0.4)
            text(cr, name, cols[0], yy, 32, WHITE, a, bold=r == 2)
            text(cr, routed, cols[1], yy, 32, col, a, align="center", bold=r == 2)
            text(cr, unc, cols[2], yy, 32, col, a, align="center", bold=r == 2)
            text(cr, added, cols[3], yy, 32, WHITE, a, align="center")
        cr.set_source_rgba(*MUTED, 0.4 * tk)
        cr.rectangle(cols[0], hy + 20, 1600, 1.5)
        cr.fill()
        a = tk * ease((t - (T_D[0] + 3.2)) / 0.5)
        text(cr, "No moved part overlaps another: KiCad finds one courtyard overlap fewer than the designer's board.", W / 2, 640, 28,
             MUTED, a, align="center")
        a = tk * ease((t - (T_D[0] + 4.4)) / 0.5)
        text(cr, "On 14 benchmark boards the router left incomplete: open connections 58 → 32.", W / 2, 740, 34, WHITE, a,
             align="center")
        text(cr, "This board and LimeSDR reach 100 % only with closure; a bigger budget alone does not.", W / 2, 795, 30, CYAN, a,
             align="center")

    # 6. end card
    k = fade(t, T_END[0], 99, 0.5)
    if k > 0:
        text(cr, "tracemaker-place board.kicad_pcb -o out.kicad_pcb --mode closure", W / 2, H / 2 - 30, 34, WHITE, k, align="center",
             mono=True)
        text(cr, "github.com/DingoOz/TraceMaker", W / 2, H / 2 + 50, 40, CYAN, k, bold=True, align="center")
    return surf


def render(D, i):
    Image.fromarray(rt.to_array(frame(D, i / FPS))).save(FRAMES / f"f_{i:04d}.png")


def _worker(args):
    lo, hi = args
    D = Data()
    for i in range(lo, hi):
        render(D, i)


if __name__ == "__main__":
    if sys.argv[1] == "still":
        D = Data()
        for s in sys.argv[2:]:
            Image.fromarray(rt.to_array(frame(D, float(s)))).save(HERE / f"still_{s}.png")
    else:
        from multiprocessing import Pool
        jobs = int(sys.argv[2]) if len(sys.argv) > 2 else 8
        FRAMES.mkdir(exist_ok=True)
        n = int(DUR * FPS)
        step = math.ceil(n / jobs)
        with Pool(jobs) as p:
            p.map(_worker, [(a, min(n, a + step)) for a in range(0, n, step)])
