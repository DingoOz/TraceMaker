#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live-viewer options for unattended route runs (bench/run.py --view, the devsite stack page).

Every board that routes gets its own viewer port (first free one from 8800) and all viewers share one token kept in
bench/results/.view-token, so the stack page can embed whichever boards are routing. The viewer binds 0.0.0.0 (LAN); the token is what keeps other
machines out of the viewer, and the page that embeds it is as open as the devsite itself. `--no-freeze`: an unattended display must not hold the scene
on a routing stall. Only the first portfolio variant streams, and the viewer does not change the result (checked: identical board with and without).

  bench/viewport.py            prints the route options of a new viewer, one per line (claims its port for 5 minutes)
"""
from __future__ import annotations

import fcntl
import json
import pathlib
import secrets
import socket
import subprocess
import time

RES = pathlib.Path(__file__).resolve().parent / "results"
LO, HI, CLAIM_S = 8800, 8899, 300
SITE_PORT = 8771  # devsite/stack.py, the page that frames the viewers


def embed_origins() -> str:
    """Origins the stack page may be opened from: this machine under each of its names and LAN addresses."""
    names = {"localhost", "127.0.0.1", socket.gethostname(), socket.gethostname() + ".local"}
    try:
        names |= set(subprocess.run(["hostname", "-I"], capture_output=True, text=True, timeout=5).stdout.split())
    except (OSError, subprocess.SubprocessError):
        pass
    return " ".join(f"http://{n}:{SITE_PORT}" for n in sorted(names) if ":" not in n)


def token() -> str:
    f = RES / ".view-token"
    if not f.exists():
        RES.mkdir(parents=True, exist_ok=True)
        f.write_text(secrets.token_hex(16))
        f.chmod(0o600)
    return f.read_text().strip()


def _free(port: int) -> bool:
    s = socket.socket()
    try:
        s.bind(("0.0.0.0", port))
        return True
    except OSError:
        return False
    finally:
        s.close()


def new_port() -> int:
    RES.mkdir(parents=True, exist_ok=True)
    with open(RES / ".view-claims", "a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        lock.seek(0)
        try:
            claims = {int(k): v for k, v in json.loads(lock.read() or "{}").items()}
        except ValueError:
            claims = {}
        now = time.time()
        claims = {p: t for p, t in claims.items() if now - t < CLAIM_S}
        for p in range(LO, HI + 1):
            if p not in claims and _free(p):
                claims[p] = now
                lock.seek(0)
                lock.truncate()
                lock.write(json.dumps(claims))
                return p
    raise RuntimeError("no free viewer port")


def route_args() -> list[str]:
    return ["--view", "--view-host", "0.0.0.0", "--view-port", str(new_port()), "--view-token", token(), "--no-freeze", "--view-embed", embed_origins()]


if __name__ == "__main__":
    print(*route_args(), sep="\n")
