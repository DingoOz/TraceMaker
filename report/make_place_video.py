#!/usr/bin/env python3
"""Placement comparison video: Freerouting routing the designer's placement next to TraceMaker placing the parts
and then routing them, with live CPU-thread strips under both panels and a closing score card.

  build/report-venv/bin/python report/make_place_video.py --board retroreflectors_SALSAFLOCK \
      --out report/compare_placement_SALSAFLOCK.mp4

Inputs in build/video_place/ (made by scripts/record_placement_video.sh, see report/VIDEO.md): human.kicad_pcb,
placed.kicad_pcb, place_events.jsonl (tracemaker-place --record), tm_events.jsonl + tm.kicad_pcb (tracemaker
route --record), fr_human.mp4/.ses/.log and fr_placed.ses/.log (bench/record_fr.py), tm_human.kicad_pcb, and the
thread samples cpu_place.json, cpu_route.json, cpu_fr.json (bench/cpu_sample.py).

Acts: (1) placement: designer's placement vs TraceMaker's placement stages (the winning candidate's recorded states,
interpolated); its thread strip is the whole placement job compressed to the act; (2) routing at 1x real time:
Freerouting's own GUI (screen recording) vs TraceMaker rendered from its event log; (3) results, every board judged
by KiCad's DRC, with two control runs (each router on the other placement) so the placement's share is visible.
"""
import argparse
import importlib.util
import json
import math
import pathlib
import re
import shutil
import subprocess

from PIL import Image, ImageDraw, ImageFont

ROOT = pathlib.Path(__file__).resolve().parent.parent
V = ROOT / "build/video_place"
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
PY = str(ROOT / "build/report-venv/bin/python")


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    m = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(m)
    return m


q = load("q", ROOT / "bench/quality.py")
mf = load("mf", ROOT / "report/make_figures.py")
rp = load("rp", ROOT / "report/render_place.py")
mv = load("mv", ROOT / "report/make_video.py")
plt = mf.plt

W, H, FPS = 1920, 1080, 15
PW, PH, PX, PY0 = 930, 700, (20, 970), 150
SY, SH = 862, 176
BG, PANEL, INK, MUTED, DIM = "#0d0f12", "#101418", "#ffffff", "#c3c2b7", "#8d8c84"
TEAL, CYAN, AMBER, RED, GREEN = "#3fb8a8", "#4fd6ff", "#e0a33a", "#e5534b", "#4cc94c"
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONTB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"
LANES = 10


def font(size, bold=False):
    return ImageFont.truetype(FONTB if bold else FONT, size)


def hexrgb(h):
    h = h.lstrip("#")
    return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))


def mix(a, b, t):
    a, b = hexrgb(a), hexrgb(b)
    return tuple(int(x + (y - x) * t) for x, y in zip(a, b))


# ---- CPU thread samples ------------------------------------------------------------------------------------------

def cpu_series(path, java_only=False):
    """[(t, sorted per-thread core shares)] of the sampled job; for Freerouting only the JVM's threads count (the
    Xvfb display and the screen recorder are measurement equipment)."""
    d = json.loads(pathlib.Path(path).read_text())
    keep = None
    if java_only:
        pids = {}
        for s in d["samples"]:
            for k in s["threads"]:
                pid, name = k.split("/")[0], k.split(" ", 1)[1]
                if name in ("VM Thread", "GC Thread#0", "C2 CompilerThre"):
                    pids[pid] = True
        keep = set(pids)
    out = []
    for s in d["samples"]:
        vals = [v for k, v in s["threads"].items() if keep is None or k.split("/")[0] in keep]
        out.append((s["t"], sorted((min(v, 1.0) for v in vals if v > 0.02), reverse=True)))
    return out, d["seconds"]


def series_at(series, t):
    best = series[0][1] if series else []
    for ts, v in series:
        if ts > t:
            break
        best = v
    return best


def cpu_summary(series, t0, t1):
    pts = [(t, sum(v)) for t, v in series if t0 <= t <= t1]
    if not pts:
        return 0.0, 0
    busy = sum(c for _, c in pts) / len(pts)
    peak = max(len([x for x in v if x > 0.3]) for t, v in series if t0 <= t <= t1)
    return busy, peak


def draw_strip(img, x, title, sub, series, t_now, t_span, t0=0.0, note=None, accent=TEAL):
    """Thread lanes under a panel: lane i is lit while at least i+1 threads are busy (brightness = that thread's
    share of a core). The history scrolls left; the big number is cores busy right now."""
    d = ImageDraw.Draw(img)
    d.rounded_rectangle([x, SY, x + PW, SY + SH], radius=10, fill=PANEL)
    d.text((x + 16, SY + 10), title, font=font(19, True), fill=INK)
    d.text((x + 16, SY + 36), sub, font=font(14), fill=DIM)
    gx0, gx1, gy0 = x + 16, x + PW - 200, SY + 62
    lane_h = 10
    if note:
        d.text((gx0, gy0 + 38), note, font=font(17), fill=MUTED)
        return
    cols = gx1 - gx0
    for lane in range(LANES):
        y = gy0 + (LANES - 1 - lane) * lane_h
        d.rectangle([gx0, y, gx1, y + lane_h - 3], fill="#181d23")
    if series is not None and t_now >= 0:
        for c in range(cols):
            tc = t0 + t_now - t_span * (1 - c / cols)
            if tc < t0:
                continue
            v = series_at(series, tc)
            for lane in range(min(LANES, len(v))):
                y = gy0 + (LANES - 1 - lane) * lane_h
                d.line([gx0 + c, y, gx0 + c, y + lane_h - 3], fill=mix("#181d23", accent, max(0.25, v[lane])))
        now = series_at(series, t0 + t_now)
        busy = sum(now)
        d.text((gx1 + 22, gy0 - 4), f"{busy:.1f}", font=font(46, True), fill=accent)
        d.text((gx1 + 22, gy0 + 50), "cores busy now", font=font(14), fill=DIM)
        d.text((gx1 + 22, gy0 + 70), f"{len([v for v in now if v > 0.3])} threads working", font=font(14), fill=DIM)
    d.text((gx0, gy0 + LANES * lane_h + 2), "1 lane = 1 CPU thread  ·  history scrolls left", font=font(12), fill=DIM)


# ---- board renders -------------------------------------------------------------------------------------------------

def view_box(board):
    edges = mf.edge_cuts(board)
    xs = [x for s in edges for x, _ in s]
    ys = [y for s in edges for _, y in s]
    x0, x1, y0, y1 = min(xs) - 2, max(xs) + 2, min(ys) - 2, max(ys) + 2
    want = PW / PH
    if (x1 - x0) / (y1 - y0) < want:
        c, w = (x0 + x1) / 2, (y1 - y0) * want
        x0, x1 = c - w / 2, c + w / 2
    else:
        c, h = (y0 + y1) / 2, (x1 - x0) / want
        y0, y1 = c - h / 2, c + h / 2
    return x0, y0, x1, y1


def drc_json(board):
    out = board.with_suffix(".drc.json")
    if not out.exists() or out.stat().st_mtime < board.stat().st_mtime:
        subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "-o", str(out), str(board)],
                       capture_output=True)
    return json.loads(out.read_text())


def render_final(board, png, bbox, w, h, label):
    """Routed board in the report style with KiCad's verdict drawn on it: router-introduced violations as red rings,
    unconnected items as dashed magenta lines."""
    dpi = 100
    fig = plt.figure(figsize=(w / dpi, h / dpi), dpi=dpi)
    fig.patch.set_facecolor(PANEL)
    ax = fig.add_axes([0, 0, 1, 1])
    mf.draw_board(ax, board, crop=bbox, lw_scale=2.0)
    ax.set_aspect("auto")
    dr = drc_json(board)
    n_err = 0
    for v in dr.get("violations", []):
        if v.get("severity") != "error":
            continue
        if not any(i.get("description", "").startswith(("Track", "Via", "Arc")) for i in v.get("items", [])):
            continue
        n_err += 1
        p = v["items"][0]["pos"]
        ax.add_patch(plt.Circle((p["x"], p["y"]), 1.1, fill=False, ec=RED, lw=2.5, zorder=20))
    for u in dr.get("unconnected_items", []):
        it = u.get("items", [])
        if len(it) >= 2:
            ax.plot([it[0]["pos"]["x"], it[1]["pos"]["x"]], [it[0]["pos"]["y"], it[1]["pos"]["y"]], color="#ff4fd8", lw=2.4,
                    ls=(0, (2, 1.5)), zorder=21)
    ax.text(0.015, 0.985, label, transform=ax.transAxes, color="white", fontsize=15, va="top",
            bbox=dict(boxstyle="round,pad=0.35", fc=PANEL, ec="none", alpha=0.85), zorder=30)
    fig.savefig(png, dpi=dpi, facecolor=PANEL)
    plt.close(fig)
    return n_err, len(dr.get("unconnected_items", []))


def fit(img, w, h):
    im = img.copy()
    im.thumbnail((w, h))
    out = Image.new("RGB", (w, h), hexrgb(PANEL))
    out.paste(im, ((w - im.width) // 2, (h - im.height) // 2))
    return out


# ---- main ----------------------------------------------------------------------------------------------------------

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="retroreflectors_SALSAFLOCK")
    ap.add_argument("--out", default=str(ROOT / "report/compare_placement_SALSAFLOCK.mp4"))
    ap.add_argument("--place-seconds", type=float, default=16.0, help="video length of the placement act")
    ap.add_argument("--fr-version", default="2.5.0-RC12")
    a = ap.parse_args()
    human, placed = V / "human.kicad_pcb", V / "placed.kicad_pcb"
    frames = V / "frames"
    if frames.exists():
        shutil.rmtree(frames)
    frames.mkdir()
    bbox = view_box(human)
    bb = ",".join(f"{v:.3f}" for v in bbox)

    # Results, each judged by KiCad's DRC against the board it routed.
    fr_h_board, fr_p_board = V / "fr_human.kicad_pcb", V / "fr_placed.kicad_pcb"
    subprocess.run(["python3", str(ROOT / "bench/ses_import.py"), str(human), str(V / "fr_human.ses"), "-o", str(fr_h_board)], check=True,
                   capture_output=True)
    subprocess.run(["python3", str(ROOT / "bench/ses_import.py"), str(placed), str(V / "fr_placed.ses"), "-o", str(fr_p_board)], check=True,
                   capture_output=True)
    m_fr = q.metrics(human, fr_h_board)
    m_tm = q.metrics(placed, V / "tm.kicad_pcb")
    m_frp = q.metrics(placed, fr_p_board)
    m_tmh = q.metrics(human, V / "tm_human.kicad_pcb")
    conns = int(re.search(r"routed \d+/(\d+)", (V / "tm_route.log").read_text()).group(1))

    def routed(m):
        return round(conns * m["completion"])

    def errs(m):
        return sum(m.get("router_errors", {}).values())

    def ratsnest(board):
        d = mf.board_json(board)
        nets = {}
        for p in d["pads"]:
            if p.get("net"):
                nets.setdefault(p["net"], []).append((p["x"] / 1e6, p["y"] / 1e6))
        return sum(rp.mst_length(v)[0] for v in nets.values() if len(v) <= 400)
    rats_h, rats_p = ratsnest(human), ratsnest(placed)
    pj = json.loads((V / "placed.json").read_text())
    moved = pj.get("moved", 0)

    # Times and thread samples.
    cpu_pl, place_s = cpu_series(V / "cpu_place.json")
    cpu_rt, route_job_s = cpu_series(V / "cpu_route.json")
    cpu_fr, fr_job_s = cpu_series(V / "cpu_fr.json", java_only=True)
    tm_d = max(json.loads(line).get("t", 0) for line in open(V / "tm_events.jsonl"))
    fr_d = mv.fr_routing_seconds(V / "fr_human.log")
    rec = re.search(r"\(([\d.]+) s of routing\)", (V / "rec_human.log").read_text())
    fr_window = float(rec.group(1)) if rec else fr_d
    fr_offset = max(0.0, fr_job_s - fr_window - 1.0)  # sampler clock at the moment the GUI recording starts
    # Routing starts when the 8 variants start: the first sample with more than two cores busy.
    route_offset = next((max(0.0, t - 0.25) for t, v in cpu_rt if sum(v) > 2.0), 0.0)
    tm_job = route_job_s - route_offset  # the portfolio runs its slower variants to their deadline
    print(f"placement {place_s:.0f} s, TM routing {tm_d:.1f} s, FR routing {fr_d:.1f} s (window {fr_window:.1f} s)")

    # Panel frame sequences.
    n_place = int(a.place_seconds * FPS)
    subprocess.run([PY, str(ROOT / "report/render_place.py"), str(V / "place_events.jsonl"), str(human), str(V / "pf"), "--frames",
                    str(n_place - 25), "--hold", "25", "--width", str(PW), "--height", str(PH), "--bbox", bb], check=True)
    subprocess.run([PY, str(ROOT / "report/render_events.py"), str(V / "tm_events.jsonl"), str(placed), str(V / "rf"), "--step",
                    f"{1 / FPS:.5f}", "--width", str(PW), "--height", str(PH), "--bbox", bb], check=True)
    fr_frames = V / "ff"
    if fr_frames.exists():
        shutil.rmtree(fr_frames)
    fr_frames.mkdir()
    vdur = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(V / "fr_human.mp4")],
                                capture_output=True, text=True).stdout)
    cut = min(fr_d + 0.5, vdur - 1.0)  # its window closes when it exits
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", str(V / "fr_human.mp4"), "-t", f"{cut:.2f}", "-vf",
                    f"fps={FPS},scale={PW}:{PH}:force_original_aspect_ratio=decrease,pad={PW}:{PH}:(ow-iw)/2:(oh-ih)/2:color=0x101418",
                    str(fr_frames / "f_%05d.png")], check=True)
    pf = sorted((V / "pf").glob("frame_*.png"))
    rf = sorted((V / "rf").glob("frame_*.png"))
    ff = sorted(fr_frames.glob("f_*.png"))
    fh, fhe = V / "final_fr.png", None
    fhe = render_final(fr_h_board, fh, bbox, PW, 470, f"Freerouting: {routed(m_fr)}/{conns} routed")
    tme = render_final(V / "tm.kicad_pcb", V / "final_tm.png", bbox, PW, 470, f"TraceMaker: {routed(m_tm)}/{conns} routed")

    n = 0

    def save(img):
        nonlocal n
        img.save(frames / f"f_{n:05d}.png")
        n += 1

    def base(act, act_name):
        img = Image.new("RGB", (W, H), hexrgb(BG))
        d = ImageDraw.Draw(img)
        d.text((W // 2, 12), "Does moving the parts help? Placement + routing on the same board", font=font(28, True), fill=INK, anchor="mt")
        d.text((W // 2, 50), f"{a.board.split('_', 1)[-1]} (PCBench tier B, {conns} connections) · TraceMaker vs Freerouting "
               f"{a.fr_version} · same machine · every result judged by KiCad's DRC", font=font(16), fill=MUTED, anchor="mt")
        for i, nm in enumerate(["1 · Placement", "2 · Routing", "3 · Result"]):
            cx = W // 2 + (i - 1) * 190
            on = i + 1 == act
            d.rounded_rectangle([cx - 85, 76, cx + 85, 102], radius=13, fill=TEAL if on else "#1b2027")
            d.text((cx, 89), nm, font=font(15, on), fill="#06110f" if on else DIM, anchor="mm")
        return img, d

    def panel_titles(d, left, left_sub, right, right_sub):
        for x, t, s in ((PX[0], left, left_sub), (PX[1], right, right_sub)):
            d.text((x + PW // 2, PY0 - 19), t, font=font(20, True), fill=INK, anchor="mb")
            d.text((x + PW // 2, PY0 - 2), s, font=font(13), fill=DIM, anchor="mb")

    # Intro card.
    for k in range(int(3.5 * FPS)):
        img = Image.new("RGB", (W, H), hexrgb(BG))
        d = ImageDraw.Draw(img)
        d.text((W // 2, 330), "Does moving the parts help routing?", font=font(54, True), fill=INK, anchor="mm")
        d.text((W // 2, 410), f"Board: {a.board.split('_', 1)[-1]} from PCBench (tier B, {conns} connections, 2 layers)",
               font=font(24), fill=MUTED, anchor="mm")
        d.text((W // 2, 520), f"Left: Freerouting {a.fr_version} routes the designer's placement (it does not place parts)",
               font=font(24), fill=INK, anchor="mm")
        d.text((W // 2, 565), "Right: TraceMaker moves the parts first, checking each candidate with its router, then routes",
               font=font(24), fill=INK, anchor="mm")
        d.text((W // 2, 680), "1 Placement   →   2 Routing (real time)   →   3 Result (KiCad DRC)", font=font(26, True), fill=TEAL,
               anchor="mm")
        save(img)

    # Act 1: placement.
    left_place = Image.open(pf[0]).convert("RGB")
    for k in range(n_place):
        img, d = base(1, "Placement")
        panel_titles(d, f"Freerouting {a.fr_version}", "keeps the designer's placement: it has no placement step",
                     "TraceMaker", f"places: {moved} parts moved · stages of the winning candidate, as recorded")
        img.paste(left_place, (PX[0], PY0))
        img.paste(Image.open(pf[min(k, len(pf) - 1)]).convert("RGB"), (PX[1], PY0))
        draw_strip(img, PX[0], "Freerouting · placement", "", None, 0, 1, note="No placement step: the board goes straight to routing.")
        tl = place_s * k / max(1, n_place - 1)
        draw_strip(img, PX[1], f"TraceMaker · placement job: {tl:.0f} of {place_s:.0f} s (shown {place_s / a.place_seconds:.0f}x faster)",
                   "8 annealing runs in parallel, then check routes (8 router variants each)", cpu_pl, tl, place_s * 0.35)
        save(img)

    # Act 2: routing at 1x.
    n_route = int((max(tm_job, fr_d) + 2.0) * FPS)
    for k in range(n_route):
        t = k / FPS
        img, d = base(2, "Routing")
        panel_titles(d, f"Freerouting {a.fr_version}", "its own window (screen recording) · 1 routing thread",
                     "TraceMaker", "rendered from its event log · 8 router variants in parallel, best kept")
        img.paste(Image.open(ff[min(k, len(ff) - 1)]).convert("RGB"), (PX[0], PY0))
        img.paste(Image.open(rf[min(k, len(rf) - 1)]).convert("RGB"), (PX[1], PY0))
        for x, dur in ((PX[0], fr_d), (PX[1], tm_d)):
            cy = PY0 + PH - 82
            d.rounded_rectangle([x + 10, cy - 6, x + 470, cy + 30], radius=8, fill=PANEL)
            d.text((x + 20, cy), f"real time {min(t, dur):4.1f} s", font=font(20), fill=INK)
            if t >= dur:
                d.text((x + 250, cy), f"routed in {dur:.1f} s", font=font(20, True), fill=GREEN)
        if t >= tm_d:
            msg = f"job ended at {tm_job:.1f} s" if t >= tm_job else "slower variants run to their deadline..."
            d.text((PX[1] + 20, PY0 + PH - 40), msg, font=font(17), fill=MUTED)
        draw_strip(img, PX[0], "Freerouting · routing", "Java: 1 routing thread; the other busy threads are the JVM's JIT compiler",
                   cpu_fr, min(t, fr_d + 0.5), 12.0, t0=fr_offset, accent=AMBER)
        draw_strip(img, PX[1], "TraceMaker · routing", "portfolio: 8 variants race on 8 threads; the best result is kept",
                   cpu_rt, min(t, tm_job), 12.0, t0=route_offset)
        save(img)

    # Act 3: results.
    fr_busy, fr_peak = cpu_summary(cpu_fr, fr_offset, fr_offset + fr_d)
    tm_busy, tm_peak = cpu_summary(cpu_rt, route_offset, route_offset + tm_job)
    pl_busy, _ = cpu_summary(cpu_pl, 0, place_s)
    rows = [
        ("Connections routed", f"{routed(m_fr)} / {conns}", f"{routed(m_tm)} / {conns}"),
        ("New KiCad DRC errors", str(errs(m_fr)), str(errs(m_tm))),
        ("Ratsnest before routing", f"{rats_h:,.0f} mm", f"{rats_p:,.0f} mm"),
        ("Track length", f"{m_fr['length_mm']:,.0f} mm", f"{m_tm['length_mm']:,.0f} mm"),
        ("Vias", str(m_fr["vias"]), str(m_tm["vias"])),
        ("Placement time", "none", f"{place_s:.0f} s"),
        ("Routing time (whole job)", f"{fr_d:.0f} s", f"{tm_job:.0f} s"),
        ("Avg cores busy (routing)", f"{fr_busy:.1f}", f"{tm_busy:.1f}  (placing {pl_busy:.1f})"),
    ]
    fin_fr, fin_tm = Image.open(fh).convert("RGB"), Image.open(V / "final_tm.png").convert("RGB")
    d_routed = routed(m_tm) - routed(m_fr)
    d_err = errs(m_tm) - errs(m_fr)
    for k in range(int(14 * FPS)):
        img, d = base(3, "Result")
        panel_titles(d, f"Freerouting {a.fr_version} on the designer's placement", "red rings: new DRC errors · magenta: still unconnected",
                     "TraceMaker: its placement, its routing", "red rings: new DRC errors · magenta: still unconnected")
        img.paste(fin_fr, (PX[0], PY0))
        img.paste(fin_tm, (PX[1], PY0))
        alpha = min(1.0, k / (0.8 * FPS))
        card = Image.new("RGB", (W - 40, 400), hexrgb(PANEL))
        cd = ImageDraw.Draw(card)
        x0s = (30, 430, 700)
        cd.text((x0s[1] + 120, 20), "Freerouting", font=font(19, True), fill=AMBER, anchor="mt")
        cd.text((x0s[2] + 150, 20), "TraceMaker", font=font(19, True), fill=TEAL, anchor="mt")
        for i, (lab, l, r) in enumerate(rows):
            y = 52 + i * 42
            if i % 2 == 0:
                cd.rectangle([20, y - 8, 1010, y + 32], fill="#141a20")
            cd.text((x0s[0], y), lab, font=font(19), fill=MUTED)
            cd.text((x0s[1] + 120, y), l, font=font(21, True), fill=INK, anchor="mt")
            cd.text((x0s[2] + 150, y), r, font=font(21, True), fill=INK, anchor="mt")
        # Headline improvements.
        hx = 1060
        cd.text((hx, 20), "TraceMaker vs Freerouting on this board", font=font(22, True), fill=INK)
        heads = [(f"{d_routed:+d}", "connections routed", GREEN if d_routed > 0 else MUTED),
                 (f"{d_err:+d}", "new DRC errors", GREEN if d_err < 0 else MUTED),
                 (f"{(m_tm['length_mm'] / m_fr['length_mm'] - 1) * 100:+.0f}%", "track length", GREEN if m_tm['length_mm'] < m_fr['length_mm'] else MUTED),
                 (f"{(m_tm['vias'] / max(1, m_fr['vias']) - 1) * 100:+.0f}%", "vias", GREEN if m_tm['vias'] < m_fr['vias'] else MUTED)]
        for i, (big, lab, col) in enumerate(heads):
            cx, cy = hx + (i % 2) * 400, 56 + (i // 2) * 100
            cd.text((cx, cy), big, font=font(54, True), fill=col)
            cd.text((cx + 4, cy + 64), lab, font=font(17), fill=MUTED)
        fe = m_fr.get("router_errors", {})
        cd.text((hx, 262), "Freerouting's errors: " + ", ".join(f"{v} {k.replace('_', ' ')}" for k, v in sorted(fe.items(), key=lambda kv: -kv[1]))
                + " (KiCad's Specctra export omits the minimum track width)", font=font(14), fill=DIM)
        cd.text((hx, 292), "Placement's own share (controls, same settings, KiCad DRC):", font=font(16, True), fill=MUTED)
        cd.text((hx, 318), f"TraceMaker's router: {routed(m_tmh)}/{conns} on the designer's placement -> {routed(m_tm)}/{conns} on its own "
                f"(ratsnest {(rats_p / rats_h - 1) * 100:+.1f}%)", font=font(16), fill=MUTED)
        cd.text((hx, 342), f"Freerouting: {routed(m_fr)}/{conns} on the designer's placement, {routed(m_frp)}/{conns} on TraceMaker's "
                f"({errs(m_frp)} new errors)", font=font(16), fill=MUTED)
        cd.text((hx, 366), "TraceMaker's placement added 0 DRC errors (checked against the designer's board).", font=font(16), fill=MUTED)
        region = img.crop((20, 640, W - 20, 1040))
        img.paste(Image.blend(region, card, alpha), (20, 640))
        save(img)

    for k in range(int(1.0 * FPS)):
        save(img)
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-framerate", str(FPS), "-i", str(frames / "f_%05d.png"), "-c:v", "libx264",
                    "-preset", "medium", "-crf", "19", "-pix_fmt", "yuv420p", "-movflags", "+faststart", a.out], check=True)
    summary = {"board": a.board, "connections": conns, "moved": moved, "ratsnest_mm": [round(rats_h, 1), round(rats_p, 1)],
               "freerouting": {k: m_fr.get(k) for k in ("completion", "router_errors", "length_mm", "vias")} | {"seconds": fr_d, "cores": round(fr_busy, 2)},
               "tracemaker": {k: m_tm.get(k) for k in ("completion", "router_errors", "length_mm", "vias")} | {"seconds": tm_d, "job_s": round(tm_job, 1), "place_s": place_s, "cores": round(tm_busy, 2), "place_cores": round(pl_busy, 2)},
               "fr_on_tm_placement": {k: m_frp.get(k) for k in ("completion", "router_errors", "length_mm", "vias")},
               "tm_on_designer_placement": {k: m_tmh.get(k) for k in ("completion", "router_errors", "length_mm", "vias")},
               "final_markers": {"fr": fhe, "tm": tme}}
    (V / "video_summary.json").write_text(json.dumps(summary, indent=1))
    print(f"wrote {a.out}: {n / FPS:.1f} s")
    print(json.dumps(summary, indent=1))


if __name__ == "__main__":
    main()
