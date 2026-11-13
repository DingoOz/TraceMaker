#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""`tracemaker daemon` (doc 19 §10) through its real socket.

  daemon_cli.py <tracemaker> <work dir>

Checked: the socket is only the owner's (0600, directory 0700); `open` copies the board, so the client may delete it; a `call` gives what the same call
gives in `tracemaker session`; `diff` returns the routed copper and, after an undo, the copper removed; `open` with the fingerprint `mark` recorded reuses
the session without a board, another fingerprint does not; a second daemon on the same socket refuses to start; bad requests are answered and the
daemon goes on; `shutdown` removes the socket.
"""
import json
import os
import pathlib
import shutil
import socket
import stat
import subprocess
import sys
import tempfile
import time

ROOT = pathlib.Path(__file__).resolve().parents[2]
PLANE_SMD = ROOT / "tests/boards/plane_smd/plane_smd.kicad_pcb"


def check(ok, what):
    if not ok:
        print("FAIL:", what)
        sys.exit(1)
    print("ok  ", what)


def req(path, obj):
    s = socket.socket(socket.AF_UNIX)
    s.settimeout(600)
    s.connect(path)
    s.sendall((json.dumps(obj) + "\n" if not isinstance(obj, str) else obj).encode())
    buf = b""
    while not buf.endswith(b"\n"):
        c = s.recv(65536)
        if not c:
            break
        buf += c
    s.close()
    return json.loads(buf)


def main():
    tm, work = pathlib.Path(sys.argv[1]).resolve(), pathlib.Path(sys.argv[2]).resolve()
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    # AF_UNIX paths are short (108 bytes): the socket lives in a short temporary directory.
    sdir = pathlib.Path(tempfile.mkdtemp(prefix="tmd-"))
    sock = str(sdir / "d" / "daemon.sock")
    d = subprocess.Popen([str(tm), "daemon", "--socket", sock, "--idle-exit", "300"], stderr=open(work / "daemon.log", "w"))
    try:
        for _ in range(100):
            if os.path.exists(sock):
                break
            time.sleep(0.05)
        check(os.path.exists(sock), "the daemon serves its socket")
        check(stat.S_IMODE(os.stat(sock).st_mode) == 0o600 and stat.S_IMODE(os.stat(os.path.dirname(sock)).st_mode) == 0o700, "socket 0600 in a 0700 directory")
        second = subprocess.run([str(tm), "daemon", "--socket", sock], capture_output=True, text=True, timeout=30)
        check(second.returncode == 1 and "already serving" in second.stderr, "a second daemon on the same socket refuses to start")
        tmp = work / "client"
        tmp.mkdir()
        shutil.copyfile(PLANE_SMD, tmp / "b.kicad_pcb")
        shutil.copyfile(PLANE_SMD.with_suffix(".kicad_pro"), tmp / "b.kicad_pro")
        o = req(sock, {"op": "open", "board": str(tmp / "b.kicad_pcb"), "key": "plane_smd", "fingerprint": "f0"})
        check(o.get("session") == 1 and o.get("reused") is False, f"open starts a session: {o}")
        shutil.rmtree(tmp)  # the session has its own copy
        listing = req(sock, {"op": "call", "session": 1, "tool": "open_connections", "args": {}})["result"]
        ref = subprocess.run([str(tm), "session", str(PLANE_SMD), "--jsonl"], input=json.dumps({"tool": "open_connections", "args": {}}) + "\n",
                             capture_output=True, text=True, timeout=300)
        check(listing == json.loads(ref.stdout.splitlines()[0]), "a call answers as `tracemaker session` does")
        net = next(c["net"] for c in listing["connections"] if c["b"] != "zone")
        r = req(sock, {"op": "call", "session": 1, "tool": "route", "args": {"nets": [net]}})["result"]
        check(r.get("committed") is True, f"route {net} through the daemon: committed")
        df = req(sock, {"op": "diff", "session": 1, "from": 0})
        check(len(df["tracks"]) > 0 and not df["removed"]["tracks"] and df["state"] == r["state"], f"diff from state 0: {len(df['tracks'])} tracks added")
        check(all(t["net"] == net and isinstance(t["start"], list) for t in df["tracks"]), "the diff is in the --emit-items layout, all of the routed net")
        check(req(sock, {"op": "mark", "session": 1, "fingerprint": "f1"}).get("ok"), "mark")
        w = req(sock, {"op": "open", "key": "plane_smd", "fingerprint": "f1"})
        check(w.get("reused") is True and w.get("session") == 1 and w.get("state") == r["state"], f"the marked fingerprint reuses the session: {w}")
        miss = req(sock, {"op": "open", "key": "plane_smd", "fingerprint": "other"})
        check("error" in miss, "another fingerprint without a board is refused")
        req(sock, {"op": "call", "session": 1, "tool": "undo", "args": {}})
        back = req(sock, {"op": "diff", "session": 1, "from": r["state"]})
        check(not back["tracks"] and len(back["removed"]["tracks"]) == len(df["tracks"]), "after an undo the diff lists the copper removed")
        check("error" in req(sock, {"op": "nope"}) and "error" in req(sock, {"op": "call", "session": 9, "tool": "board_info"}) and "error" in req(sock, "{bad json\n"),
              "bad requests are answered")
        check(req(sock, {"op": "status"})["sessions"][0]["key"] == "plane_smd", "status lists the session")
        check(req(sock, {"op": "shutdown"}).get("ok"), "shutdown")
        d.wait(timeout=30)
        check(d.returncode == 0 and not os.path.exists(sock), "the daemon exits and removes its socket")
    finally:
        if d.poll() is None:
            d.kill()
        shutil.rmtree(sdir, ignore_errors=True)
    print("OK")


if __name__ == "__main__":
    main()
