#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""`tracemaker tui` and `route --config` (doc 02 §2.1, D89).

  tui_options.py <tracemaker> <work dir>

The options screen is driven twice: with scripted keys (`--keys`, no terminal) and through a pseudo-terminal, as a
user would. Checked: the command it prints, the options file it saves, that `route --config FILE` routes the same
board byte for byte as the same options typed out, that the command line wins over the file, that a run started
from the screen is that same run, and that bad files and a missing terminal are refused with a message.
"""
import os
import pathlib
import pty
import select
import subprocess
import sys
import time

BOARD = """(kicad_pcb (version 20240108) (generator "pcbnew")
  (layers (0 "F.Cu" signal) (1 "In1.Cu" signal) (2 "In2.Cu" signal) (31 "B.Cu" signal) (37 "F.SilkS" user)
    (44 "Edge.Cuts" user))
  (net 0 "") (net 1 "SIG") (net 2 "GND")
  (footprint "R" (layer "F.Cu") (at 4 5)
    (property "Reference" "R1" (at 0 0) (layer "F.SilkS"))
    (pad "1" thru_hole circle (at 0 -2) (size 1.2 1.2) (drill 0.6) (layers "*.Cu") (net 1 "SIG"))
    (pad "2" thru_hole circle (at 0 2) (size 1.2 1.2) (drill 0.6) (layers "*.Cu") (net 2 "GND"))
  )
  (footprint "R" (layer "F.Cu") (at 16 5)
    (property "Reference" "R2" (at 0 0) (layer "F.SilkS"))
    (pad "1" thru_hole circle (at 0 -2) (size 1.2 1.2) (drill 0.6) (layers "*.Cu") (net 1 "SIG"))
    (pad "2" thru_hole circle (at 0 2) (size 1.2 1.2) (drill 0.6) (layers "*.Cu") (net 2 "GND"))
  )
  (gr_rect (start 0 0) (end 20 10) (layer "Edge.Cuts") (stroke (width 0.1) (type solid)))
)
"""
FIXED = ["--work", "200000", "--variants", "1", "--no-kb", "--no-gpu"]  # one deterministic router, no GPU needed
# The same options as keys: filter to the option by name, change it, clear the filter.
FIXED_KEYS = ("/--work<enter><enter>200000<enter><esc>/--variants<enter><enter>1<enter><esc>"
              "/--no-kb<enter><space><esc>/--no-gpu<enter><space><esc>")
COST_KEYS = "/--layer-cost<enter><enter>F.Cu=50,B.Cu=50<enter><esc>/pair-twists<enter><space><space><esc>"


def main():
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    board = work / "in.kicad_pcb"
    board.write_text(BOARD)
    # The screen also reads the user's and the board folder's options files: keep it away from the real ones.
    os.environ["XDG_CONFIG_HOME"] = str(work / "xdg")
    (work / "tracemaker.conf").unlink(missing_ok=True)
    failures = []

    def check(ok, what):
        if not ok:
            failures.append(what)

    def run(*args, **kw):
        return subprocess.run([tm, *map(str, args)], capture_output=True, text=True, timeout=300, **kw)

    def route(out, *args):
        r = run("route", board, "-o", work / out, *args)
        check(r.returncode == 0, f"route {out} {args}: exit {r.returncode}: {r.stderr[-300:]}")
        return (work / out).read_bytes() if (work / out).exists() else b""

    # 1. Scripted keys: the printed command and the saved file.
    cfg = work / "options.toml"
    cfg.unlink(missing_ok=True)
    r = run("tui", board, "-o", work / "x.kicad_pcb", "--config", cfg, "--keys", FIXED_KEYS + COST_KEYS + "s<enter>")
    want = (f"tracemaker route {board} --output {work / 'x.kicad_pcb'} --work 200000 --no-pair-twists --no-gpu --no-kb "
            "--variants 1 --layer-cost F.Cu=50,B.Cu=50")
    check(r.returncode == 0 and r.stdout.strip() == want, f"scripted keys printed:\n  {r.stdout.strip()}\nwanted:\n  {want}")
    saved = [l for l in cfg.read_text().splitlines() if l and not l.startswith("#")] if cfg.exists() else []
    check(saved == ["work = 200000", "no-pair-twists = true", "no-gpu = true", "no-kb = true", "variants = 1",
                    'layer-cost = "F.Cu=50,B.Cu=50"'], f"saved options file: {saved}")

    # 2. The file routes like the typed options, and differently from the defaults (so it was really applied).
    typed = route("typed.kicad_pcb", *FIXED, "--layer-cost", "F.Cu=50,B.Cu=50", "--no-pair-twists")
    filed = route("filed.kicad_pcb", "--config", cfg)
    plain = route("plain.kicad_pcb", *FIXED)
    check(typed and typed == filed, "route --config differs from the same options on the command line")
    check(typed != plain, "the test options do not change the routing: the comparison proves nothing")
    # 3. The command line wins over the file.
    over = route("over.kicad_pcb", "--config", cfg, "--layer-cost", "F.Cu=1")
    check(over == plain, "an option on the command line did not override the options file")

    # 4. The screen starts from a file; `name = false` is the flag's other spelling; a run from the screen is that run.
    cfg2 = work / "start.toml"
    cfg2.write_text("# comment\nwork = 200000\nvariants = 1\nno-kb\nno-gpu = true\npair-twists = false\n"
                    "layer-cost = [\"F.Cu=50\", \"B.Cu=50\"]\n")
    r = run("tui", board, "-o", work / "run.kicad_pcb", "--config", cfg2, "--keys", "r")
    check(r.returncode == 0 and (work / "run.kicad_pcb").exists() and (work / "run.kicad_pcb").read_bytes() == typed,
          f"a run from the screen differs from the typed command (exit {r.returncode}): {r.stderr[-300:]}")
    check(route("filed2.kicad_pcb", "--config", cfg2) == typed, "route --config with false/array/bare-name lines differs")
    # Without an output the screen refuses to run and says why.
    r = run("tui", board, "--keys", "r")
    check("cannot run yet" in r.stderr and "--output is required" in r.stderr, f"no refusal without --output: {r.stderr!r}")

    # 4b. Scopes: the global file under the project file under --config; a save goes to the target scope only.
    glob = work / "xdg" / "tracemaker" / "route.conf"
    glob.parent.mkdir(parents=True, exist_ok=True)
    glob.write_text("work = 100\nseed = 5\nvariants = 1\n")
    proj = work / "tracemaker.conf"
    proj.write_text("work = 200\n")
    r = run("tui", board, "-o", work / "s.kicad_pcb", "--keys", "p")
    want = f"tracemaker route {board} --output {work / 's.kicad_pcb'} --work 200 --seed 5 --variants 1"
    check(r.stdout.strip() == want, f"global under project:\n  {r.stdout.strip()}\nwanted:\n  {want}")
    # No --config: edits and `s` go to the project file (the default target); the global file is left alone.
    r = run("tui", board, "-o", work / "s.kicad_pcb", "--keys", "/--seed<enter><enter><bs>9<enter><esc>s<enter>")
    check(r.returncode == 0 and "seed = 9" in proj.read_text() and "work = 200" in proj.read_text(),
          f"project save: {proj.read_text()!r}")
    check("seed" not in proj.read_text().replace("seed = 9", "") and "variants" not in proj.read_text(),
          f"project file took global values: {proj.read_text()!r}")
    check(glob.read_text() == "work = 100\nseed = 5\nvariants = 1\n", f"global file changed: {glob.read_text()!r}")
    # Tab moves the target: the same edit now lands in the global file.
    r = run("tui", board, "-o", work / "s.kicad_pcb", "--keys", "<tab>/--variants<enter><enter><bs>2<enter><esc>s<enter>")
    check("variants = 2" in glob.read_text(), f"global save: {glob.read_text()!r} {r.stderr!r}")
    proj.unlink()
    glob.unlink()

    # 5. Bad files are refused, with the file and line.
    for text, needle in (("work = 200000\nno-such-option = 1\n", "bad.toml:2: no route option named no-such-option"),
                         ("micro-vias = false\n", "has no opposite"), ("no-gpu = maybe\n", "must be true or false"),
                         ("work = lots\n", "--work")):
        bad = work / "bad.toml"
        bad.write_text(text)
        r = run("route", board, "-o", work / "bad.kicad_pcb", "--config", bad)
        check(r.returncode != 0 and needle in r.stderr, f"options file {text!r}: exit {r.returncode}, stderr {r.stderr!r}")

    # 6. No terminal: a message, not a hang.
    r = run("tui", board, stdin=subprocess.DEVNULL, start_new_session=True)
    check(r.returncode == 1 and "needs a terminal" in r.stderr, f"without a terminal: exit {r.returncode}, {r.stderr!r}")

    # 7. A real terminal: arrow keys, the filter and `p`. The screen goes to the terminal, the command to stdout.
    master, slave = pty.openpty()

    def own_terminal():
        os.setsid()
        import fcntl
        import termios
        fcntl.ioctl(slave, termios.TIOCSCTTY, 0)

    p = subprocess.Popen([tm, "tui", str(board), "-o", "o.kicad_pcb"], stdin=slave, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                         preexec_fn=own_terminal, text=True)
    os.close(slave)
    screen = b""

    def drain(seconds):
        nonlocal screen
        end = time.time() + seconds
        while time.time() < end:
            if select.select([master], [], [], 0.05)[0]:
                try:
                    screen += os.read(master, 65536)
                except OSError:
                    return

    drain(1.0)
    for keys in (b"/soft-zones\r", b" ", b"\x1b", b"\x1b[B", b"\x1b[A", b"p"):
        os.write(master, keys)
        drain(0.4)
    try:
        out, _ = p.communicate(timeout=20)
    except subprocess.TimeoutExpired:
        p.kill()
        out, _ = p.communicate()
        failures.append("the screen did not end on `p`")
    os.close(master)
    check(out.strip() == f"tracemaker route {board} --output o.kicad_pcb --soft-zones", f"terminal session printed {out!r}")
    check(b"tracemaker route options" in screen and b"--soft-zones" in screen and b"\x1b[?1049l" in screen,
          "the terminal did not show the screen or was not restored")

    for f in failures:
        print("FAIL:", f)
    print("tui_options:", "FAIL" if failures else "OK")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
