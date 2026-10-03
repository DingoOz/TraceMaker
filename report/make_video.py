#!/usr/bin/env python3
"""Compose the side-by-side routing video: TraceMaker (rendered from its event recording) next to Freerouting's own
GUI recordings, all at the same speed-up, with real-time clocks and a closing metrics table.

  build/report-venv/bin/python report/make_video.py --board AmpOne_dev-AmpOne --speed 4 --out report/compare_AmpOne.mp4

Inputs (from the steps in report/VIDEO.md): build/video/tm_frames/ (report/render_events.py at step = speed/15 s),
build/video/tm_events.jsonl and tm.kicad_pcb, build/video/fr25.mp4/.ses/.log and fr19.mp4/.ses/.log
(bench/record_fr.py).
"""
import argparse
import importlib.util
import json
import pathlib
import re
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
V = ROOT / "build/video"
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
spec = importlib.util.spec_from_file_location("q", ROOT / "bench/quality.py")
q = importlib.util.module_from_spec(spec)
spec.loader.exec_module(q)

import matplotlib  # noqa: E402

matplotlib.use("agg")
import matplotlib.pyplot as plt  # noqa: E402

W, H, PW, GAP, PY = 1920, 1080, 620, 30, 150
FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"
FONTB = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"


def fr_routing_seconds(log: pathlib.Path) -> float:
    """Routing time from Freerouting's log: first to last timestamp before the session is saved."""
    ts = []
    for line in log.read_text(errors="replace").splitlines():
        m = re.match(r"(\d{4}-\d\d-\d\d) (\d\d):(\d\d):(\d\d\.\d+)", line)
        if m:
            ts.append(int(m.group(2)) * 3600 + int(m.group(3)) * 60 + float(m.group(4)))
        if "Saving" in line or "Saved" in line:
            break
    return ts[-1] - ts[0] if len(ts) > 1 else 0.0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default="AmpOne_dev-AmpOne")
    ap.add_argument("--speed", type=float, default=4.0)
    ap.add_argument("--fps", type=int, default=15)
    ap.add_argument("--out", default=str(ROOT / "report/compare_AmpOne.mp4"))
    a = ap.parse_args()
    unrouted = FIX / a.board / "unrouted.kicad_pcb"

    # Durations (seconds of routing) and final quality of exactly the recorded runs.
    tm_end = max(json.loads(line).get("t", 0) for line in open(V / "tm_events.jsonl"))
    runs = [("TraceMaker", V / "tm.kicad_pcb", tm_end, "8 threads, rendered from its event log")]
    for tag, ver, name in (("fr25", "2.5.0-RC12", "Freerouting “2.5.0-RC12”"), ("fr19", "1.9.0", "Freerouting 1.9.0")):
        board = V / f"{tag}.kicad_pcb"
        subprocess.run(["python3", str(ROOT / "bench/ses_import.py"), str(unrouted), str(V / f"{tag}.ses"), "-o", str(board)],
                       check=True, capture_output=True)
        runs.append((name, board, fr_routing_seconds(V / f"{tag}.log"), "1 thread, its own GUI (screen recording)"))
    metrics = [q.metrics(unrouted, r[1]) for r in runs]
    longest = max(r[2] for r in runs) / a.speed
    total = longest + 8.0

    # Background: title, panel labels, footer. End card: metrics table.
    dpi = 100
    fig = plt.figure(figsize=(W / dpi, H / dpi), dpi=dpi)
    fig.patch.set_facecolor("#0d0f12")
    fig.text(0.5, 0.955, f"Same board, three routers: {a.board.split('_')[0]} (PCBench tier C, 321 connections)", ha="center",
             color="white", fontsize=26, weight="bold")
    fig.text(0.5, 0.915, f"Played at {a.speed:g}× real time on the same machine; each panel stops when that router finished. "
             "Every result judged by KiCad's DRC.", ha="center", color="#c3c2b7", fontsize=15)
    for i, (name, _, _, how) in enumerate(runs):
        x = (i * (PW + GAP) + PW / 2) / W
        fig.text(x, 1 - (PY - 22) / H, name, ha="center", color="white", fontsize=19, weight="bold")
        fig.text(x, 1 - (PY - 2) / H, how, ha="center", color="#8d8c84", fontsize=12)
    fig.text(0.5, 0.012, "Freerouting \u201c2.5.0-RC12\u201d is the jar of that name in Freerouting's own benchmark set; it reports itself "
             "as v2.4.2-SNAPSHOT (built 2026-09-29). Freerouting routes from KiCad's Specctra export, which omits board-edge "
             "clearance and minimum track width.", ha="center", color="#8d8c84", fontsize=10)
    fig.savefig(V / "bg.png", dpi=dpi, facecolor=fig.get_facecolor())
    plt.close(fig)

    rows = [("KiCad DRC errors added", lambda m: str(sum(m.get("router_errors", {}).values()))),
            ("Connections routed", lambda m: f"{100 * m['completion']:.1f}%"),
            ("Track length (mm)", lambda m: f"{m['length_mm']:,.0f}"),
            ("Vias", lambda m: str(m["vias"])),
            ("Bends", lambda m: f"{m['bends']:,}"),
            ("Routing time", lambda m: "")]
    fig = plt.figure(figsize=(W / dpi, 330 / dpi), dpi=dpi)
    fig.patch.set_facecolor("#0d0f12")
    ax = fig.add_axes([0.02, 0.02, 0.96, 0.96])
    ax.set_axis_off()
    cells = [[lab] + [f(m) if lab != "Routing time" else f"{r[2]:.0f} s" for (m, r) in zip(metrics, runs)] for lab, f in rows]
    t = ax.table(cellText=cells, colLabels=[""] + [r[0] for r in runs], cellLoc="center", colWidths=[0.28, 0.24, 0.24, 0.24],
                 bbox=[0.08, 0.02, 0.84, 0.96])
    t.auto_set_font_size(False)
    t.set_fontsize(15)
    for (r, c), cell in t.get_celld().items():
        cell.set_edgecolor("#2e2e2b")
        cell.set_facecolor("#16181c" if r % 2 else "#1d2026")
        cell.get_text().set_color("white" if r else "#c3c2b7")
        if r == 0 or c == 0:
            cell.get_text().set_weight("bold")
        if c == 0 and r > 0:
            cell.get_text().set_ha("left")
            cell.PAD = 0.04
    fig.savefig(V / "card.png", dpi=dpi, facecolor=fig.get_facecolor())
    plt.close(fig)

    # ffmpeg composition. Freerouting's window closes when it exits, so its recordings are cut 1.5 s before their end.
    s = a.speed
    tm_d, fr25_d, fr19_d = runs[0][2], runs[1][2], runs[2][2]
    def vdur(p):
        return float(subprocess.run(["ffprobe", "-v", "error", "-show_entries", "format=duration", "-of", "csv=p=0", str(p)],
                                    capture_output=True, text=True).stdout)
    cut25, cut19 = min(fr25_d, vdur(V / "fr25.mp4") - 1.5), min(fr19_d, vdur(V / "fr19.mp4") - 1.5)
    def clock(i, d):
        x = i * (PW + GAP) + 20
        y = PY + 484 + 18
        return (f"drawtext=fontfile={FONT}:fontsize=24:fontcolor=white:x={x}:y={y}:"
                f"text='real time %{{eif\\:min(t*{s}\\,{d:.1f})\\:d}} s',"
                f"drawtext=fontfile={FONTB}:fontsize=24:fontcolor=0x4cc94c:x={x + 300}:y={y}:"
                f"text='finished in {d:.0f} s':enable='gte(t\\,{d / s:.2f})'")
    fc = (
        f"[0:v]scale={W}:{H}[bg];"
        f"[1:v]scale={PW}:-2,tpad=stop_mode=clone:stop_duration={total}[p0];"
        f"[2:v]trim=end={cut25:.2f},setpts=(PTS-STARTPTS)/{s},fps={a.fps},scale={PW}:-2,tpad=stop_mode=clone:stop_duration={total}[p1];"
        f"[3:v]trim=end={cut19:.2f},setpts=(PTS-STARTPTS)/{s},fps={a.fps},scale={PW}:-2,tpad=stop_mode=clone:stop_duration={total}[p2];"
        f"[bg][p0]overlay=0:{PY}:shortest=0[a];[a][p1]overlay={PW + GAP}:{PY}[b];[b][p2]overlay={2 * (PW + GAP)}:{PY}[c];"
        f"[c]{clock(0, tm_d)},{clock(1, fr25_d)},{clock(2, fr19_d)}[d];"
        f"[d][4:v]overlay=0:{H - 372}:enable='gte(t\\,{longest:.2f})'[out]"
    )
    cmd = ["ffmpeg", "-loglevel", "error", "-y",
           "-loop", "1", "-t", f"{total:.2f}", "-i", str(V / "bg.png"),
           "-framerate", str(a.fps), "-i", str(V / "tm_frames/frame_%05d.png"),
           "-i", str(V / "fr25.mp4"), "-i", str(V / "fr19.mp4"),
           "-loop", "1", "-t", f"{total:.2f}", "-i", str(V / "card.png"),
           "-filter_complex", fc, "-map", "[out]", "-t", f"{total:.2f}", "-r", str(a.fps),
           "-c:v", "libx264", "-preset", "medium", "-crf", "20", "-pix_fmt", "yuv420p", "-movflags", "+faststart", a.out]
    subprocess.run(cmd, check=True)
    print(f"wrote {a.out}: {total:.1f} s; routing times TM {tm_d:.0f} s, FR25 {fr25_d:.0f} s, FR19 {fr19_d:.0f} s")
    for (name, _, d, _), m in zip(runs, metrics):
        print(name, {k: m.get(k) for k in ("completion", "router_errors", "length_mm", "vias", "bends")}, f"{d:.0f} s")


if __name__ == "__main__":
    main()
