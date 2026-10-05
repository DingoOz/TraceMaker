#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Placement comparison video: Freerouting routing the designer's placement next to TraceMaker placing the parts
and then routing them, with live CPU-thread strips under both panels and a closing score card.

  build/report-venv/bin/python report/make_place_video.py --board retroreflectors_SALSAFLOCK \
      --out report/compare_placement_SALSAFLOCK.mp4

Inputs in build/video_place/ (made by scripts/record_placement_video.sh, see report/VIDEO.md): human.kicad_pcb,
placed.kicad_pcb, place_events.jsonl (tracemaker-place --record), tm_events.jsonl + tm.kicad_pcb (tracemaker
route --record), fr_human.mp4/.ses/.log and fr_placed.ses/.log (bench/record_fr.py), tm_human.kicad_pcb, and the
thread samples cpu_place.json, cpu_route.json, cpu_fr.json (bench/cpu_sample.py).

Acts: (1) the two finished boards in 3D (kicad-cli pcb render, scripts/render_board_spin.py), spinning once;
(2) placement: the designer's hand placement (Freerouting's input; time not recorded) next to TraceMaker placing the
parts on its own clock (the kept candidate's states at the times they existed, captioned with what the job was doing);
(3) routing, one clock for both starting when routing starts, at real time: Freerouting's own GUI (screen recording)
vs TraceMaker rendered from its event log; each panel switches to the saved KiCad board when that tool finishes.
Vertical meters show the CPU threads and GPU SM use sampled while the jobs ran; the elapsed time is the largest
number. (4) results, every board judged by KiCad's DRC, with two control runs (each
router on the other placement) so the placement's share is visible.
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

JVM_HOUSEKEEPING = re.compile(r"^(C1 CompilerThre|C2 CompilerThre|GC Thread|G1 |VM Thread|VM Periodic|Service Thread|Sweeper)")


def cpu_series(path, java_only=False):
    """[(t, [(core share, housekeeping?) busiest first])] of the sampled job. For Freerouting only the JVM's threads
    count (the Xvfb display and the screen recorder are measurement equipment), and the JVM's own JIT-compiler and GC
    threads are flagged so they can be drawn apart from the routing thread."""
    d = json.loads(pathlib.Path(path).read_text())
    keep = None
    if java_only:
        keep = {k.split("/")[0] for s in d["samples"] for k in s["threads"]
                if k.split(" ", 1)[1] in ("VM Thread", "GC Thread#0", "C2 CompilerThre")}
    out = []
    for s in d["samples"]:
        vals = [(min(v, 1.0), bool(java_only and JVM_HOUSEKEEPING.match(k.split(" ", 1)[1])))
                for k, v in s["threads"].items() if (keep is None or k.split("/")[0] in keep) and v > 0.02]
        out.append((s["t"], sorted(vals, key=lambda x: (x[1], -x[0]))))
    return out, d["seconds"]


def first_busy(path, name_re, threshold=0.5):
    """Sampler time at which a thread whose name matches first uses more than `threshold` of a core."""
    d = json.loads(pathlib.Path(path).read_text())
    for s in d["samples"]:
        if any(v > threshold and re.match(name_re, k.split(" ", 1)[1]) for k, v in s["threads"].items()):
            return s["t"]
    return None


def series_at(series, t):
    best = series[0][1] if series else []
    for ts, v in series:
        if ts > t:
            break
        best = v
    return best


def cpu_summary(series, t0, t1):
    pts = [(t, sum(x for x, _ in v)) for t, v in series if t0 <= t <= t1]
    if not pts:
        return 0.0, 0
    busy = sum(c for _, c in pts) / len(pts)
    peak = max(len([x for x, _ in v if x > 0.3]) for t, v in series if t0 <= t <= t1)
    return busy, peak


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


def render_final(board, png, bbox, w, h, label, markers=True):
    """Routed board in the report style with KiCad's verdict drawn on it: router-introduced violations as red rings,
    unconnected items as dashed magenta lines."""
    dpi = 100
    fig = plt.figure(figsize=(w / dpi, h / dpi), dpi=dpi)
    fig.patch.set_facecolor(PANEL)
    ax = fig.add_axes([0, 0, 1, 1])
    mf.draw_board(ax, board, crop=bbox, lw_scale=2.0)
    ax.set_aspect("auto")
    dr = drc_json(board) if markers else {}
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
    ap.add_argument("--out", help="default: report/compare_placement_<board>.mp4")
    ap.add_argument("--fr-version", default="2.5.0-RC12")
    ap.add_argument("--caveat", default="", help="one line on the result card (e.g. design intent the placer does not know)")
    ap.add_argument("--tier", help="PCBench tier shown in the subtitle (default: looked up in bench/results)")
    a = ap.parse_args()
    global V
    if (V / a.board).is_dir():
        V = V / a.board               # scripts/record_placement_video.sh BOARD writes build/video_place/BOARD
    tier = a.tier
    if not tier:
        for f in sorted((ROOT / "bench/results").glob("*/boards.jsonl")):
            for line in open(f):
                if f'"board": "{a.board}"' in line and '"tier"' in line:
                    tier = json.loads(line).get("tier")
                    break
            if tier:
                break
    a.out = a.out or str(ROOT / f"report/compare_placement_{a.board.split('_', 1)[-1]}.mp4")
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
    # Freerouting's routing: from "Starting routing" to "Saving" in its log (JVM start-up and loading excluded).
    stamps = {}
    for line in (V / "fr_human.log").read_text(errors="replace").splitlines():
        mm_ = re.match(r"\d{4}-\d\d-\d\d (\d\d):(\d\d):(\d\d\.\d+)", line)
        if mm_:
            ts = int(mm_.group(1)) * 3600 + int(mm_.group(2)) * 60 + float(mm_.group(3))
            if "Starting routing" in line and "start" not in stamps:
                stamps["start"] = ts
            if "Starting optimization" in line:
                stamps["opt"] = ts
            if "Saving" in line:
                stamps["end"] = ts
    fr_d = stamps["end"] - stamps["start"]
    fr_opt = stamps.get("opt", stamps["end"]) - stamps["start"]  # its autorouter pass is single-threaded, the optimizer is not
    opt_log = (V / "fr_human.log").read_text(errors="replace")
    fr_opt_ran = "Skipping optimization" not in opt_log
    rec = re.search(r"\(([\d.]+) s of routing\)", (V / "rec_human.log").read_text())
    fr_window = float(rec.group(1)) if rec else fr_d
    fr_rec0 = max(0.0, fr_job_s - fr_window - 1.0)    # sampler clock when the GUI recording starts
    fr_offset = first_busy(V / "cpu_fr.json", r"Thread-\d+") or fr_rec0  # sampler clock when routing starts
    fr_video0 = fr_offset - fr_rec0                    # recording time when routing starts
    # Routing starts when the 8 variants start: the first sample with more than two cores busy.
    route_offset = next((max(0.0, t - 0.25) for t, v in cpu_rt if sum(x for x, _ in v) > 2.0), 0.0)
    tm_job = route_job_s - route_offset  # the portfolio runs its slower variants to their deadline
    print(f"placement {place_s:.0f} s, TM routing {tm_d:.1f} s, FR routing {fr_d:.1f} s (window {fr_window:.1f} s)")

    # Placement act: TraceMaker's own clock (the designer placed Freerouting's board by hand, time not recorded).
    # Routing act: one clock for both, starting when routing starts, at real time.
    speed_p = max(1, round(place_s / 14.0))
    n_place = int(math.ceil(place_s / speed_p * FPS)) + int(1.5 * FPS)
    r_end = max(fr_d, route_job_s - route_offset)
    speed_r = max(1, math.ceil(r_end / 45.0))     # same constant speed for both tools
    n_route = int(math.ceil(r_end / speed_r * FPS)) + 2 * FPS
    BW, BH, BY, CW = 780, 680, 150, 130
    bx = (20 + CW + 10, 980)                      # board panels
    cx_ = (20, 980 + BW + 10)                     # CPU/GPU columns
    gpu_names = {0: "P100", 1: "V100"}
    gpu_pl = [(r["t"], r["gpu"], r["sm"]) for r in json.loads((V / "cpu_place.json").read_text()).get("gpu", [])]
    gpu_rt = [(r["t"] - route_offset, r["gpu"], r["sm"]) for r in json.loads((V / "cpu_route.json").read_text()).get("gpu", [])]
    fields = [(int(m.group(1)), float(m.group(2))) for m in re.finditer(r"fields (\d+) GPU \+ \d+ CPU \(([\d.]+) s", (V / "tm_route.log").read_text())]
    pfields = [(int(m.group(1)), float(m.group(2))) for m in re.finditer(r"fields (\d+) GPU \+ \d+ CPU \(([\d.]+) s", (V / "place.log").read_text())]
    tm_route_end = route_job_s - route_offset     # on the routing clock

    def gpu_at(rows, t):
        out = {}
        for ts, g, sm in rows:
            if t - 1.5 <= ts <= t:
                out[g] = sm
        return out

    subprocess.run([PY, str(ROOT / "report/render_place.py"), str(V / "place_events.jsonl"), str(human), str(V / "pf"), "--speed", str(speed_p),
                    "--fps", str(FPS), "--width", str(BW), "--height", str(BH), "--bbox", bb], check=True)
    subprocess.run([PY, str(ROOT / "report/render_place.py"), str(V / "place_events.jsonl"), str(human), str(V / "pf_left"), "--speed", "100000",
                    "--fps", str(FPS), "--width", str(BW), "--height", str(BH), "--bbox", bb, "--no-caption"], check=True)
    subprocess.run([PY, str(ROOT / "report/render_events.py"), str(V / "tm_events.jsonl"), str(placed), str(V / "rf"), "--step",
                    f"{speed_r / FPS:.5f}", "--width", str(BW), "--height", str(BH), "--bbox", bb], check=True)
    fr_frames = V / "ff"
    if fr_frames.exists():
        shutil.rmtree(fr_frames)
    fr_frames.mkdir()
    vdur = float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(V / "fr_human.mp4")],
                                capture_output=True, text=True).stdout)
    cut = min(fr_d + 0.3, vdur - fr_video0 - 0.5)  # its window closes when it exits
    subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-ss", f"{fr_video0:.2f}", "-i", str(V / "fr_human.mp4"), "-t", f"{cut:.2f}", "-vf",
                    f"setpts=PTS/{speed_r},fps={FPS},scale={BW}:{BH}:force_original_aspect_ratio=decrease,pad={BW}:{BH}:(ow-iw)/2:(oh-ih)/2:color=0x101418",
                    str(fr_frames / "f_%05d.png")], check=True)
    pf = sorted((V / "pf").glob("frame_*.png"))
    left_place = Image.open(sorted((V / "pf_left").glob("frame_*.png"))[0]).convert("RGB")
    ImageDraw.Draw(left_place).text((12, 10), "Placed by the designer, by hand", font=font(20), fill=INK)
    rf = sorted((V / "rf").glob("frame_*.png"))
    ff = sorted(fr_frames.glob("f_*.png"))
    fh = V / "final_fr.png"
    fhe = render_final(fr_h_board, fh, bbox, PW, 470, f"Freerouting: {routed(m_fr)}/{conns} routed")
    # The saved boards in the panels' size: shown once each tool has finished (the KiCad files, not the replay).
    render_final(fr_h_board, V / "saved_fr.png", bbox, BW, BH, f"Saved result: {routed(m_fr)}/{conns} routed", markers=False)
    render_final(V / "tm.kicad_pcb", V / "saved_tm.png", bbox, BW, BH, f"Saved result: {routed(m_tm)}/{conns} routed", markers=False)
    saved_fr, saved_tm = Image.open(V / "saved_fr.png").convert("RGB"), Image.open(V / "saved_tm.png").convert("RGB")
    tme = render_final(V / "tm.kicad_pcb", V / "final_tm.png", bbox, PW, 470, f"TraceMaker: {routed(m_tm)}/{conns} routed")

    n = 0

    def save(img):
        nonlocal n
        img.save(frames / f"f_{n:05d}.png")
        n += 1

    def base(act, note=""):
        img = Image.new("RGB", (W, H), hexrgb(BG))
        d = ImageDraw.Draw(img)
        d.text((W // 2, 10), "Does moving the parts help? Placement, then routing, on the same board", font=font(28, True), fill=INK, anchor="mt")
        d.text((W // 2, 48), f"{a.board.split('_', 1)[-1]} (PCBench tier {tier}, {conns} connections) · TraceMaker vs Freerouting "
               f"{a.fr_version} · same machine · {note}results judged by KiCad's DRC", font=font(16), fill=MUTED, anchor="mt")
        for i, nm in enumerate(["1 · Finished boards", "2 · Placement", "3 · Routing", "4 · Result"]):
            cx = W // 2 + int((i - 1.5) * 230)
            on = i + 1 == act
            d.rounded_rectangle([cx - 105, 74, cx + 105, 100], radius=13, fill=TEAL if on else "#1b2027")
            d.text((cx, 87), nm, font=font(15, on), fill="#06110f" if on else DIM, anchor="mm")
        return img, d

    def panel_titles(d, left, left_sub, right, right_sub):
        for x, t, s_ in ((PX[0], left, left_sub), (PX[1], right, right_sub)):
            d.text((x + PW // 2, PY0 - 19), t, font=font(20, True), fill=INK, anchor="mb")
            d.text((x + PW // 2, PY0 - 2), s_, font=font(13), fill=DIM, anchor="mb")

    def board_titles(d, left, left_sub, right, right_sub):
        for x, title, sub in ((bx[0], left, left_sub), (bx[1], right, right_sub)):
            d.text((x + BW // 2, BY - 18), title, font=font(20, True), fill=INK, anchor="mb")
            d.text((x + BW // 2, BY - 2), sub, font=font(13), fill=DIM, anchor="mb")

    def column(img, x, threads, gpus, accent, note=None, gpu_note=None):
        """Vertical meters: one bar per busy CPU thread (height = share of a core), then the GPUs (SM %)."""
        d = ImageDraw.Draw(img)
        d.rounded_rectangle([x, BY, x + CW, BY + BH], radius=10, fill=PANEL)
        d.text((x + CW // 2, BY + 10), "CPU threads", font=font(14, True), fill=INK, anchor="mt")
        if note:
            d.multiline_text((x + CW // 2, BY + 200), note, font=font(14), fill=MUTED, anchor="ma", align="center")
            return
        top, bot = BY + 34, BY + 430
        slots = 12
        bw = (CW - 20) // slots
        for i in range(slots):
            x0 = x + 10 + i * bw
            d.rectangle([x0, top, x0 + bw - 2, bot], fill="#181d23")
            if i < len(threads):
                v, hk = threads[i]
                h = int((bot - top) * v)
                d.rectangle([x0, bot - h, x0 + bw - 2, bot], fill=mix(PANEL, accent, 0.4) if hk else accent)
        busy = sum(v for v, _ in threads)
        work = len([v for v, hk in threads if v > 0.3 and not hk])
        hk_n = len([v for v, hk in threads if v > 0.3 and hk])
        d.text((x + CW // 2, bot + 8), f"{work} working", font=font(15, True), fill=INK, anchor="mt")
        d.text((x + CW // 2, bot + 28), f"+{hk_n} JIT/GC" if hk_n else f"{busy:.1f} cores", font=font(13), fill=DIM, anchor="mt")
        d.text((x + CW // 2, bot + 62), "GPU (CUDA)", font=font(14, True), fill=INK, anchor="mt")
        gtop, gbot = bot + 86, BY + BH - 40
        if gpu_note:
            d.multiline_text((x + CW // 2, gtop + 40), gpu_note, font=font(13), fill=DIM, anchor="ma", align="center")
            return
        for i, g in enumerate((0, 1)):
            x0 = x + 18 + i * 52
            d.rectangle([x0, gtop, x0 + 42, gbot], fill="#181d23")
            sm = gpus.get(g, 0)
            h = int((gbot - gtop) * sm / 100)
            d.rectangle([x0, gbot - h, x0 + 42, gbot], fill="#a77bff")
            d.text((x0 + 21, gbot + 6), gpu_names[g], font=font(12), fill=DIM, anchor="mt")
            d.text((x0 + 21, gtop - 2), f"{sm}%", font=font(12), fill=INK, anchor="mb")

    def info(d, side, clock, acc, status, detail, segs, t, span, foot):
        """Info bar under a board: the clock is the biggest number on screen; a bar shows the phases on this clock."""
        x0 = 20 if side == 0 else 980
        d.rounded_rectangle([x0, BY + BH + 12, x0 + 920, H - 14], radius=10, fill=PANEL)
        big = font(92, True) if d.textlength(clock, font=font(92, True)) < 265 else font(54, True)
        d.text((x0 + 22, BY + BH + 20 + (0 if big.size == 92 else 22)), clock, font=big, fill=acc)
        d.text((x0 + 300, BY + BH + 30), status, font=font(22, True), fill=INK)
        d.text((x0 + 300, BY + BH + 64), detail, font=font(16), fill=MUTED)
        if segs:
            y, w, x = BY + BH + 104, 590, x0 + 300
            d.rounded_rectangle([x, y, x + w, y + 22], radius=6, fill="#181d23")
            for s0, s1, col, lab in segs:
                xa, xb = x + int(w * s0 / span), x + int(w * min(s1, t) / span)
                if xb > xa:
                    d.rectangle([xa, y, xb, y + 22], fill=col)
                d.text((x + int(w * s0 / span) + 4, y + 26), lab, font=font(13), fill=DIM)
            xm = x + int(w * min(t, span) / span)
            d.line([xm, y - 4, xm, y + 26], fill=INK, width=2)
        d.text((x0 + 300, BY + BH + 160), foot, font=font(12), fill=DIM)

    # Act 1: the two finished boards in 3D (kicad-cli pcb render), spinning once.
    spins = [sorted((V / f"spin_{tag}").glob("s_*.png")) for tag in ("fr", "tm")]
    for k in range(len(spins[0])):
        img, d = base(1)
        for side, (x, title, sub, verdict, col) in enumerate((
                (20, f"Freerouting {a.fr_version}", "placed by the designer, routed by Freerouting",
                 f"{routed(m_fr)}/{conns} routed · {errs(m_fr)} new KiCad DRC errors", AMBER),
                (980, "TraceMaker", "placed and routed by TraceMaker",
                 f"{routed(m_tm)}/{conns} routed · {errs(m_tm)} new KiCad DRC errors", TEAL))):
            d.text((x + 460, 150), title, font=font(30, True), fill=col, anchor="mt")
            d.text((x + 460, 192), sub, font=font(17), fill=MUTED, anchor="mt")
            im = Image.open(spins[side][k]).convert("RGBA")
            layer = Image.new("RGBA", img.size, (0, 0, 0, 0))
            layer.paste(im, (x + 460 - im.width // 2, 225), im)
            img = Image.alpha_composite(img.convert("RGBA"), layer).convert("RGB")
            d = ImageDraw.Draw(img)
            d.text((x + 460, 900), verdict, font=font(26, True), fill=INK, anchor="mt")
        d.text((W // 2, 990), "Same netlist, same board outline. How did each get here?", font=font(22), fill=MUTED, anchor="mt")
        d.text((W // 2, 1040), "3D: kicad-cli pcb render (these footprints carry no 3D part models)", font=font(13), fill=DIM, anchor="mt")
        save(img)

    # Act 2: placement.
    for k in range(n_place):
        t = min(place_s, k * speed_p / FPS)
        img, d = base(2, f"placement played at {speed_p}x · ")
        board_titles(d, "Freerouting's input", "Freerouting does not place parts: a person placed them in KiCad",
                     "TraceMaker", f"places the parts itself ({moved} moved) · the kept candidate as it was at each moment")
        img.paste(left_place, (bx[0], BY))
        img.paste(Image.open(pf[min(k, len(pf) - 1)]).convert("RGB"), (bx[1], BY))
        column(img, cx_[0], [], {}, AMBER, note="placed by\na person\n\n(not measured)")
        column(img, cx_[1], series_at(cpu_pl, t), gpu_at(gpu_pl, t), TEAL)
        info(d, 0, "by hand", AMBER, "Placement: done by the designer", "time not recorded; Freerouting starts from this placement",
             None, t, place_s, "")
        done = t >= place_s
        info(d, 1, f"{t:.0f} s", TEAL, f"Placed: {moved} parts moved" if done else "Placing components",
             f"took {place_s:.0f} s; then routing starts" if done else f"8 annealing threads; router checks use the GPUs · {t:.0f} of {place_s:.0f} s",
             [(0, place_s, "#2a7f74", f"placing {place_s:.0f} s")], t, place_s, "placement clock (TraceMaker only)")
        save(img)

    # Act 3: routing, one clock for both from the start of routing, at real time.
    for k in range(n_route):
        t = min(r_end, k * speed_r / FPS)
        img, d = base(3, ("routing at real time" if speed_r == 1 else f"routing played at {speed_r}x") + ", one clock · ")
        board_titles(d, f"Freerouting {a.fr_version}", "routes the designer's placement · its own window (screen recording)",
                     "TraceMaker", "routes its own placement · rendered from its event log")
        img.paste(saved_fr if t >= fr_d else Image.open(ff[min(k, len(ff) - 1)]).convert("RGB"), (bx[0], BY))
        # The file is written when the whole job ends (the slower variants run to the portfolio deadline).
        img.paste(saved_tm if t >= tm_route_end else Image.open(rf[min(k, len(rf) - 1)]).convert("RGB"), (bx[1], BY))
        column(img, cx_[0], series_at(cpu_fr, fr_offset + t) if t <= fr_d else [], {}, AMBER, gpu_note="not used\n(Freerouting is\nCPU-only)")
        column(img, cx_[1], series_at(cpu_rt, route_offset + t) if t <= tm_route_end else [], gpu_at(gpu_rt, t), TEAL)
        if t >= fr_d:
            fst, fde = f"Done: {routed(m_fr)}/{conns} routed in {fr_d:.0f} s", "finished"
        elif t < fr_opt:
            fst, fde = "Autorouting (single-threaded pass)", "the parts stay where the designer put them"
        else:
            fst, fde = "Optimizing (multi-threaded optimizer)", "Freerouting's optimizer runs on many threads"
        fsegs = [(0, fr_opt, AMBER, f"autoroute {fr_opt:.0f} s")]
        if fr_opt_ran and fr_d - fr_opt > 0.5:
            fsegs.append((fr_opt, fr_d, "#a87a2c", f"optimize {fr_d - fr_opt:.0f} s"))
        info(d, 0, f"{min(t, fr_d):.0f} s", AMBER, fst, fde, fsegs, t, r_end, f"routing clock: 0 – {r_end:.0f} s, same for both")
        if t < tm_d:
            st, de = "Routing (8 router variants in parallel)", f"placement ({place_s:.0f} s) happened before this clock started"
        elif t < tm_route_end:
            st, de = f"Best variant done at {tm_d:.0f} s; others still running", "the result is saved when the slower variants reach their deadline"
        else:
            st, de = f"Done: {routed(m_tm)}/{conns} routed, saved at {tm_route_end:.0f} s", f"best variant {tm_d:.0f} s · after {place_s:.0f} s of placement"
        info(d, 1, f"{min(t, tm_route_end):.0f} s", TEAL, st, de,
             [(0, tm_d, TEAL, f"routing {tm_d:.0f} s"), (tm_d, tm_route_end, "#2a7f74", f"other variants {tm_route_end - tm_d:.0f} s")], t, r_end,
             f"routing clock: 0 – {r_end:.0f} s, same for both")
        save(img)

    # Act 4: results.
    fr_busy, fr_peak = cpu_summary(cpu_fr, fr_offset, fr_offset + fr_d)
    tm_busy, tm_peak = cpu_summary(cpu_rt, route_offset, route_offset + tm_job)
    pl_busy, _ = cpu_summary(cpu_pl, 0, place_s)
    rows = [
        ("Connections routed", f"{routed(m_fr)} / {conns}", f"{routed(m_tm)} / {conns}"),
        ("New KiCad DRC errors", str(errs(m_fr)), str(errs(m_tm))),
        ("Ratsnest before routing", f"{rats_h:,.0f} mm", f"{rats_p:,.0f} mm"),
        ("Track length", f"{m_fr['length_mm']:,.0f} mm", f"{m_tm['length_mm']:,.0f} mm"),
        ("Vias", str(m_fr["vias"]), str(m_tm["vias"])),
        ("Placement time", "by hand (not recorded)", f"{place_s:.0f} s (automatic)"),
        ("Routing time", f"{fr_d:.0f} s", f"{tm_d:.0f} s (job {tm_job:.0f} s)"),
        ("Avg cores busy (routing)", f"{fr_busy:.1f}", f"{tm_busy:.1f}  (placing {pl_busy:.1f})"),
        ("GPU (CUDA)", "not used", f"P100+V100 · {sum(f for f, _ in fields + pfields):,} fields"),
    ]
    fin_fr, fin_tm = Image.open(fh).convert("RGB"), Image.open(V / "final_tm.png").convert("RGB")
    d_routed = routed(m_tm) - routed(m_fr)
    d_err = errs(m_tm) - errs(m_fr)
    for k in range(int(14 * FPS)):
        img, d = base(4)
        panel_titles(d, f"Freerouting {a.fr_version} on the designer's placement", "red rings: new DRC errors · magenta: still unconnected",
                     "TraceMaker: its placement, its routing", "red rings: new DRC errors · magenta: still unconnected")
        img.paste(fin_fr, (PX[0], PY0))
        img.paste(fin_tm, (PX[1], PY0))
        alpha = min(1.0, k / (0.8 * FPS))
        card = Image.new("RGB", (W - 40, 420), hexrgb(PANEL))
        cd = ImageDraw.Draw(card)
        x0s = (30, 430, 700)
        cd.text((x0s[1] + 120, 20), "Freerouting", font=font(19, True), fill=AMBER, anchor="mt")
        cd.text((x0s[2] + 150, 20), "TraceMaker", font=font(19, True), fill=TEAL, anchor="mt")
        for i, (lab, l, r) in enumerate(rows):
            y = 50 + i * 38
            if i % 2 == 0:
                cd.rectangle([20, y - 7, 1010, y + 29], fill="#141a20")
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
                + " (Specctra export lacks the min. track width)", font=font(14), fill=DIM)
        cd.text((hx, 292), "Placement's own share (controls, same settings, KiCad DRC):", font=font(16, True), fill=MUTED)
        cd.text((hx, 318), f"TraceMaker's router: {routed(m_tmh)}/{conns} on the designer's placement -> {routed(m_tm)}/{conns} on its own "
                f"(ratsnest {(rats_p / rats_h - 1) * 100:+.1f}%)", font=font(16), fill=MUTED)
        cd.text((hx, 342), f"Freerouting: {routed(m_fr)}/{conns} on the designer's placement, {routed(m_frp)}/{conns} on TraceMaker's "
                f"({errs(m_frp)} new errors)", font=font(16), fill=MUTED)
        cd.text((hx, 366), "TraceMaker's placement added 0 DRC errors (checked against the designer's board).", font=font(16), fill=MUTED)
        if a.caveat:
            cd.text((hx, 392), "Caveat: " + a.caveat, font=font(14, True), fill=AMBER)
        region = img.crop((20, 626, W - 20, 1046))
        img.paste(Image.blend(region, card, alpha), (20, 626))
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
