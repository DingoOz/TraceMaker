#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Screen-record Freerouting's own GUI while it routes a PCBench board (for comparison videos).

  build/report-venv/bin/python bench/record_fr.py AmpOne_dev-AmpOne 2.5.0-RC12 out.mp4 [--display 97] [--dsn other.dsn]

Starts a private Xvfb display, runs Freerouting with its published benchmark settings and its GUI on, moves
secondary windows (the settings panel) off the captured area, and records the main window with ffmpeg until
Freerouting saves its result and exits. Needs xvfb, ffmpeg and python-xlib (build/report-venv).
"""
import argparse
import pathlib
import subprocess
import time

from Xlib import display as xdisplay

ROOT = pathlib.Path(__file__).resolve().parent.parent
FRB = ROOT / "bench/data/freerouting/scripts/benchmark"
JAVA = next(iter(sorted((ROOT / "build/tools").glob("jdk-25*/bin/java"))))


def windows(d):
    out = []
    for c in d.screen().root.query_tree().children:
        try:
            a, g = c.get_attributes(), c.get_geometry()
            if a.map_state and g.width > 50 and g.height > 50:
                out.append((c, g, c.get_wm_name() or ""))
        except Exception:  # noqa: BLE001 - windows come and go
            pass
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("version")
    ap.add_argument("out")
    ap.add_argument("--display", type=int, default=97)
    ap.add_argument("--fps", type=int, default=10)
    ap.add_argument("--timeout", default="00:30:00")
    ap.add_argument("--dsn", help="route this .dsn instead of the board's unrouted.dsn (e.g. from bench/dsn_place.py)")
    a = ap.parse_args()
    disp = f":{a.display}"
    xvfb = subprocess.Popen(["Xvfb", disp, "-screen", "0", "2600x1100x24"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    time.sleep(1.5)
    out = pathlib.Path(a.out)
    ses = out.with_suffix(".ses")
    log = open(out.with_suffix(".log"), "w")
    cmd = [str(JAVA), "-Xmx6g", "-jar", str(FRB / "binaries" / f"freerouting-{a.version}.jar"),
           "-de", a.dsn or str(FRB / "fixtures/PCBench" / a.board / "unrouted.dsn"), "-do", str(ses),
           "--router.max_threads=1", f"--router.job_timeout={a.timeout}", "--router.autorouter.max_passes=500",
           "--router.optimizer.enabled=true", "--router.fanout.enabled=true"]
    if not a.version.startswith("1."):
        cmd += ["--api_server.enabled=false", "--gui.enabled=true"]
    fr = subprocess.Popen(cmd, stdout=log, stderr=subprocess.STDOUT, env={"DISPLAY": disp, "PATH": "/usr/bin:/bin"})
    d = xdisplay.Display(disp)
    main_geom, ff, t_start = None, None, time.time()
    while fr.poll() is None:
        for w, g, name in windows(d):
            if "Freerouting" in name:
                main_geom = (g.x, g.y, g.width - g.width % 2, g.height - g.height % 2)
            elif g.x < 1500:  # settings panels and dialogs: move them out of the captured area
                w.configure(x=1700, y=0)
                d.sync()
        if main_geom and ff is None:
            x, y, w_, h_ = main_geom
            ff = subprocess.Popen(["ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab", "-framerate", str(a.fps),
                                   "-video_size", f"{w_}x{h_}", "-i", f"{disp}+{x},{y}", "-c:v", "libx264", "-preset", "veryfast",
                                   "-crf", "23", "-pix_fmt", "yuv420p", str(out)], stdin=subprocess.PIPE)
            t_start = time.time()
        time.sleep(0.2)
    elapsed = time.time() - t_start
    if ff:
        time.sleep(1.0)
        ff.communicate(b"q")
    xvfb.terminate()
    print(f"recorded {out} ({elapsed:.1f} s of routing), exit {fr.returncode}")


if __name__ == "__main__":
    main()
