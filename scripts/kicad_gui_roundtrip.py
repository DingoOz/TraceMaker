#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Round trip of the TraceMaker plugin in a running KiCad 10 GUI, without a desktop (roadmap M11, doc 08 §6).

  build/plugin-venv/bin/python scripts/kicad_gui_roundtrip.py
      [--board bench/data/kicad/demos/multichannel/multichannel_mixer-unrouted.kicad_pcb]
      [--work-dir build/gui] [--route-args "--time 60 --threads 8 --no-kb"] [--place MODE] [--package tracemaker-V-pcm-linux-x86_64.zip]

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
    ap.add_argument("--package", help="run the plugin from this PCM package (scripts/make_release.py) unpacked, with the engine it carries and no TRACEMAKER variable")
    ap.add_argument("--nets", default="", help="route these nets one after the other with the 'route selected nets' action through the daemon "
                    "(comma separated; doc 19 §10) instead of the whole board")
    ap.add_argument("--refill", default="", choices=["", "after", "in-commit"],
                    help="refill zones with the routes (TRACEMAKER_REFILL=1) and count the undos back to the input. KiCad 10.0.6 records a refill as a step "
                         "of its own: 2 undos. in-commit: the refill called inside the open commit (D185: still 2 undos, and it fills before the copper "
                         "is pushed; the plugin does not do it)")
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
        plugin_py = ROOT / "kicad_plugin/tracemaker_route.py"
        if a.package:  # what KiCad's Plugin and Content Manager unpacks: no executable bits kept, the engine is the bundled one
            import zipfile
            with zipfile.ZipFile(a.package) as z:
                z.extractall(g / "pkg")
            plugin_py = g / "pkg/plugins/tracemaker_route.py"
            env.pop("TRACEMAKER")
            env.pop("PYTHONPATH", None)
            print(f"plugin from {a.package}: engine {g / 'pkg/plugins/bin/tracemaker'} (executable bit: {os.access(g / 'pkg/plugins/bin/tracemaker', os.X_OK)})")
        if a.nets:
            return nets_roundtrip(a, g, sock, env, board, check, ctrl, shot, src) if ok else 1
        if a.place:
            env["TRACEMAKER_PLACE"] = a.place
        if a.refill:
            return refill_roundtrip(a, g, sock, env, board, check, ctrl, plugin_py)
        env.update(BROWSER="true", TRACEMAKER_REPORT="project")  # the report is kept next to the board; no browser is opened
        p = subprocess.run([sys.executable, str(plugin_py)], env=env, capture_output=True, text=True, timeout=3600)
        print("\n".join(line for line in p.stdout.splitlines() if line.startswith(("routed", "TraceMaker", "power widths"))))
        rep = g / "proj" / (src.stem + ".tracemaker-report.html")
        check(rep.exists() and rep.stat().st_size > 1000, f"the run report was written next to the board ({rep.name})")
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


def fill_signature(board) -> str:
    """A digest of every zone's filled polygons (what a refill changes)."""
    import hashlib
    h = hashlib.sha256()
    for z in sorted(board.get_zones(), key=lambda z: z.proto.SerializeToString(deterministic=True)):
        for layer, polys in sorted(z.filled_polygons.items()):
            h.update(str(layer).encode())
            for poly in polys:
                h.update(poly.proto.SerializeToString(deterministic=True))
    return h.hexdigest()


def refill_roundtrip(a, g, sock, env, board, check, ctrl, plugin_py) -> int:
    """--refill after|in-commit: routes with a zone refill and counts the undos it takes to get the input's copper and fills back."""
    from kipy import KiCad
    t0, v0, f0 = len(board.get_tracks()), len(board.get_vias()), fill_signature(board)
    check(bool(board.get_zones()), f"the board has zones ({len(board.get_zones())})")
    env = dict(env, TRACEMAKER_REFILL="1", TRACEMAKER_REPORT="off")
    if a.refill == "in-commit":
        env["TRACEMAKER_REFILL_IN_COMMIT"] = "1"  # honoured only by the plugin version of the D185 experiment
    p = subprocess.run([sys.executable, str(plugin_py)], env=env, capture_output=True, text=True, timeout=3600)
    print("\n".join(line for line in p.stdout.splitlines() if line.startswith("TraceMaker")))
    if p.stderr.strip():
        print(p.stderr[-800:])
    check(p.returncode == 0, "the plugin ran against the live board")
    board = KiCad(socket_path=sock).get_board()
    t1, f1 = len(board.get_tracks()), fill_signature(board)
    check(t1 != t0, f"copper added: tracks {t0} -> {t1}")
    check(f1 != f0, "the fills changed (the refill ran)")
    undos = 0
    for _ in range(3):
        ctrl("z")
        undos += 1
        board = KiCad(socket_path=sock).get_board()
        t, v, f = len(board.get_tracks()), len(board.get_vias()), fill_signature(board)
        print(f"after {undos} undo(s): tracks {t}, vias {v}, fills {'as before' if f == f0 else 'changed'}")
        if (t, v, f) == (t0, v0, f0):
            break
    check((t, v, f) == (t0, v0, f0), f"{undos} undo(s) bring back the input's copper and fills ({a.refill})")
    return 0


def drc_of(path: pathlib.Path) -> tuple:
    js = path.with_suffix(".drc.json")
    subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", str(js), str(path)], capture_output=True, cwd=path.parent)
    d = json.loads(js.read_text())
    return len(d.get("unconnected_items", [])), collections.Counter(v["type"] for v in d["violations"] if v["severity"] == "error")


def nets_roundtrip(a, g, sock, env, board, check, ctrl, shot, src) -> int:
    """--nets: each net is selected (one of its pads) and routed by kicad_plugin/tracemaker_route_nets.py through one daemon. Checks that
    each action adds copper, that the second and later actions reuse the daemon's session (warm), that one undo removes only the last net's
    copper, and KiCad's DRC as for the whole-board action. Prints each action's wall time (the latency the M18 gate states)."""
    from kipy import KiCad

    env = dict(env, TRACEMAKER_SOCKET=str(g / "daemon.sock"))
    plugin_py = ROOT / "kicad_plugin/tracemaker_route_nets.py"
    counts, times, warm = [], [], []
    ok = True
    outer = check

    def check(cond: bool, what: str) -> None:  # noqa: F811 - the caller's check, also remembered here
        nonlocal ok
        outer(cond, what)
        ok = ok and cond

    try:
        for net in [n for n in a.nets.split(",") if n]:
            board = KiCad(socket_path=sock).get_board()
            pads = [p for p in board.get_pads() if p.net is not None and p.net.name == net]
            if not pads:
                check(False, f"net {net} has pads on the board")
                return 1
            board.clear_selection()
            board.add_to_selection(pads[:1])
            before = (len(board.get_tracks()), len(board.get_vias()))
            t0 = time.time()
            p = subprocess.run([sys.executable, str(plugin_py)], env=env, capture_output=True, text=True, timeout=1800)
            times.append(time.time() - t0)
            print("\n".join(line for line in p.stdout.splitlines() if line.startswith("TraceMaker")))
            if p.returncode != 0:
                print(p.stderr[-1500:])
            check(p.returncode == 0, f"route selected nets ({net}) ran against the live board in {times[-1]:.1f} s")
            ok = ok and p.returncode == 0
            board = KiCad(socket_path=sock).get_board()
            after = (len(board.get_tracks()), len(board.get_vias()))
            if "nothing added" in p.stdout:  # a net already routed as far as it goes: the board must be unchanged
                check(after == before, f"{net}: nothing added, board unchanged")
            else:
                check(after[0] > before[0], f"{net}: tracks {before[0]} -> {after[0]}, vias {before[1]} -> {after[1]}")
                counts.append((before, after))
            warm.append("warm session" in p.stdout)
        check(not warm[0], "the first action starts a session")
        if len(warm) > 1:
            check(all(warm[1:]), "later actions reuse the daemon's session (warm)")
        shot("nets-routed.png")
        ctrl("z")
        board = KiCad(socket_path=sock).get_board()
        now = (len(board.get_tracks()), len(board.get_vias()))
        check(now == counts[-1][0], f"one undo removes only the last added net's copper: {now} (expected {counts[-1][0]})")
        ctrl("y")
        board = KiCad(socket_path=sock).get_board()
        check((len(board.get_tracks()), len(board.get_vias())) == counts[-1][1], "redo brings it back")
        out = g / "proj/after_gui.kicad_pcb"
        board.save_as(str(out), overwrite=True, include_project=True)
        (u0, e0), (u1, e1) = drc_of(g / "proj" / src.name), drc_of(out)
        new = {t: n - e0.get(t, 0) for t, n in e1.items() if n > e0.get(t, 0)}
        check(u1 < u0, f"KiCad DRC of the board saved from the GUI: unconnected {u0} -> {u1}")
        check(not new, f"no new DRC errors ({dict(new) or 'none'})")
        print("latency (s): " + ", ".join(f"{t:.2f}" for t in times))
    finally:
        try:
            import socket as so
            c = so.socket(so.AF_UNIX)
            c.connect(str(g / "daemon.sock"))
            c.sendall(b'{"op": "shutdown"}\n')
            c.recv(100)
            c.close()
        except OSError:
            pass
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
