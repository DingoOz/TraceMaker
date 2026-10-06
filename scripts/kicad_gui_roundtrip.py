#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Round trip of the TraceMaker plugin in a running KiCad 10 GUI, without a desktop (roadmap M11, doc 08 §6).

  build/plugin-venv/bin/python scripts/kicad_gui_roundtrip.py
      [--board bench/data/kicad/demos/multichannel/multichannel_mixer-unrouted.kicad_pcb]
      [--work-dir build/gui] [--route-args "--time 60 --threads 8 --no-kb"] [--place MODE]

What it does: starts Xvfb, runs `pcbnew <board>` from the kicad/kicad Docker image on that display with the IPC API
enabled, clicks through KiCad's first-run wizard, runs kicad_plugin/tracemaker_route.py against the live board, and
checks through the API and KiCad's own DRC that
  * the plugin added tracks and vias to the open board;
  * one Ctrl+Z removes all of them and Ctrl+Y brings them back (one commit);
  * the board saved from the GUI has fewer unconnected items and no new DRC errors.
Needs: docker with the KiCad image, Xvfb, and a Python with kicad-python, python-xlib and Pillow (build/plugin-venv).
Exit 0 on success, 1 on a failed check, 77 when a prerequisite is missing.
"""
import argparse
import collections
import json
import os
import pathlib
import shutil
import subprocess
import sys
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
DISPLAY = ":97"
IMAGE = os.environ.get("TRACEMAKER_KICAD_IMAGE", "kicad/kicad:10.0.6")
NAME = "tmk-gui-roundtrip"


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--board", default=str(ROOT / "bench/data/kicad/demos/multichannel/multichannel_mixer-unrouted.kicad_pcb"))
    ap.add_argument("--work-dir", default=str(ROOT / "build/gui"))
    ap.add_argument("--route-args", default="--time 60 --threads 8 --no-kb")
    ap.add_argument("--place", default="", help="also let the plugin move footprints first (TRACEMAKER_PLACE: refine, auto, ...)")
    a = ap.parse_args()
    try:
        from kipy import KiCad
        from PIL import ImageGrab
        from Xlib import X, XK, display
        from Xlib.ext import xtest
    except ImportError as e:
        print(f"SKIP: {e} (run with build/plugin-venv/bin/python; pip install kicad-python python-xlib pillow)")
        return 77
    if not shutil.which("Xvfb") or not shutil.which("docker") or not pathlib.Path(a.board).exists():
        print("SKIP: Xvfb, docker or the board is missing")
        return 77
    g = pathlib.Path(a.work_dir).resolve()
    shutil.rmtree(g, ignore_errors=True)
    for d in ("sock", "tmp", "home/.config/kicad/10.0", "proj"):
        (g / d).mkdir(parents=True)
    src = pathlib.Path(a.board)
    for f in src.parent.iterdir():  # the whole project: rules, libraries
        if f.is_dir():
            shutil.copytree(f, g / "proj" / f.name)
        else:
            shutil.copy2(f, g / "proj" / f.name)
    pro = list((g / "proj").glob("*.kicad_pro"))
    if pro and not (g / "proj" / (src.stem + ".kicad_pro")).exists():
        shutil.copy2(pro[0], g / "proj" / (src.stem + ".kicad_pro"))
    (g / "home/.config/kicad/10.0/kicad_common.json").write_text(json.dumps({"api": {"enable_server": True, "interpreter_path": "/usr/bin/python3"}}))
    xvfb = subprocess.Popen(["Xvfb", DISPLAY, "-screen", "0", "1600x1200x24", "-nolisten", "tcp"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    subprocess.run(["docker", "rm", "-f", NAME], capture_output=True)
    ok = True

    def check(cond: bool, what: str) -> None:
        nonlocal ok
        print(("ok   " if cond else "FAIL ") + what, flush=True)
        ok = ok and cond

    try:
        time.sleep(2)
        home = str(pathlib.Path.home())
        subprocess.run(["docker", "run", "-d", "--name", NAME, "-u", f"{os.getuid()}:{os.getgid()}", "-e", f"HOME={g}/home", "-e", f"DISPLAY={DISPLAY}",
                        "-v", "/tmp/.X11-unix:/tmp/.X11-unix", "-v", f"{home}:{home}", "-v", f"{g}/sock:/tmp/kicad", "-w", f"{g}/proj", IMAGE,
                        "pcbnew", f"{g}/proj/{src.name}"], check=True, capture_output=True)
        dpy = display.Display(DISPLAY)

        def click(x: int, y: int) -> None:
            xtest.fake_input(dpy, X.MotionNotify, x=x, y=y)
            dpy.sync()
            time.sleep(0.2)
            xtest.fake_input(dpy, X.ButtonPress, 1)
            xtest.fake_input(dpy, X.ButtonRelease, 1)
            dpy.sync()
            time.sleep(1.0)

        def ctrl(key: str) -> None:
            m, k = dpy.keysym_to_keycode(XK.string_to_keysym("Control_L")), dpy.keysym_to_keycode(XK.string_to_keysym(key))
            click(660, 400)  # focus the canvas
            xtest.fake_input(dpy, X.KeyPress, m)
            xtest.fake_input(dpy, X.KeyPress, k)
            dpy.sync()
            time.sleep(0.05)
            xtest.fake_input(dpy, X.KeyRelease, k)
            xtest.fake_input(dpy, X.KeyRelease, m)
            dpy.sync()
            time.sleep(2.0)

        def shot(name: str) -> None:
            ImageGrab.grab(xdisplay=DISPLAY).save(g / name)

        sock = f"ipc://{g}/sock/api.sock"
        for _ in range(60):
            if (g / "sock/api.sock").exists():
                break
            time.sleep(1)
        check((g / "sock/api.sock").exists(), "KiCad started and opened its API socket")
        time.sleep(15)
        # First start: the welcome wizard (two pages; Next / Finish sit at the same place) blocks the API until done.
        board = None
        for _ in range(6):
            try:
                board = KiCad(socket_path=sock).get_board()
                break
            except Exception:  # noqa: BLE001 - "KiCad is not ready to reply" while a dialog is open
                click(992, 807)
                time.sleep(5)
        check(board is not None, "the API answers (first-run wizard dismissed)")
        if board is None:
            shot("failed.png")
            return 1

        def ref(f) -> str:
            return f.reference_field.text.value

        def pos(f) -> tuple:
            return (f.position.x, f.position.y, round(f.orientation.degrees, 3))

        t0, v0 = len(board.get_tracks()), len(board.get_vias())
        pos0 = {ref(f): pos(f) for f in board.get_footprints()}
        shot("1-before.png")
        env = dict(os.environ, KICAD_API_SOCKET=sock, TMPDIR=str(g / "tmp"), TRACEMAKER=str(ROOT / "build/release/src/app/tracemaker"),
                   TRACEMAKER_NO_BINDINGS="1", TRACEMAKER_ARGS=a.route_args)
        if a.place:
            env["TRACEMAKER_PLACE"] = a.place
        p = subprocess.run([sys.executable, str(ROOT / "kicad_plugin/tracemaker_route.py")], env=env, capture_output=True, text=True, timeout=3600)
        print("\n".join(line for line in p.stdout.splitlines() if line.startswith(("routed", "TraceMaker"))))
        if p.returncode != 0:
            print(p.stderr[-1500:])
        check(p.returncode == 0, "the plugin ran against the live board")
        board = KiCad(socket_path=sock).get_board()
        t1, v1 = len(board.get_tracks()), len(board.get_vias())
        check(t1 != t0, f"copper changed in the open board: tracks {t0} -> {t1}; vias {v0} -> {v1}")
        if a.place:
            moved = sum(1 for f in board.get_footprints() if pos0.get(ref(f)) != pos(f))
            check(moved > 0, f"footprints moved in the open board: {moved}")
        shot("2-routed.png")
        ctrl("z")
        if a.place:
            ctrl("z")  # the zone refill after moved footprints is a step of its own
        board = KiCad(socket_path=sock).get_board()
        t2, v2 = len(board.get_tracks()), len(board.get_vias())
        back = all(pos0.get(ref(f)) == pos(f) for f in board.get_footprints())
        check((t2, v2) == (t0, v0) and back, (f"two undos (zone refill, then the commit)" if a.place else "one undo") + f" remove the whole result: {t2} tracks, {v2} vias" + (", footprints back" if a.place else ""))
        ctrl("y")
        if a.place:
            ctrl("y")
        board = KiCad(socket_path=sock).get_board()
        check((len(board.get_tracks()), len(board.get_vias())) == (t1, v1), "redo brings it back")
        out = g / "proj/after_gui.kicad_pcb"
        board.save_as(str(out), overwrite=True, include_project=True)

        def drc(path: pathlib.Path) -> tuple:
            js = path.with_suffix(".drc.json")
            subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", str(js), str(path)], capture_output=True, cwd=path.parent)
            d = json.loads(js.read_text())
            return len(d.get("unconnected_items", [])), collections.Counter(v["type"] for v in d["violations"] if v["severity"] == "error")

        (u0, e0), (u1, e1) = drc(g / "proj" / src.name), drc(out)
        new = {t: n - e0.get(t, 0) for t, n in e1.items() if n > e0.get(t, 0)}
        check(u1 < u0, f"KiCad DRC of the board saved from the GUI: unconnected {u0} -> {u1}")
        check(not new, f"no new DRC errors ({dict(new) or 'none'})")
        shot("3-final.png")
        return 0 if ok else 1
    finally:
        subprocess.run(["docker", "rm", "-f", NAME], capture_output=True)
        xvfb.terminate()


if __name__ == "__main__":
    raise SystemExit(main())
