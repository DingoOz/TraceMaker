#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live-viewer options for unattended route runs (bench/run.py --view, the devsite stack page).

Every board that routes gets its own viewer port (first free one from 8800) and all viewers share one token kept in
bench/results/.view-token, so the stack page can embed whichever boards are routing. The viewer binds 0.0.0.0 (LAN); the token is what keeps other
machines out of the viewer, and the page that embeds it is as open as the devsite itself. `--no-freeze`: an unattended display must not hold the scene
on a routing stall. Only the first portfolio variant streams, and the viewer does not change the result (checked: identical board with and without).

  bench/viewport.py            prints the route options of a new viewer, one per line (claims its port for 5 minutes)

A claim only bridges the moment between choosing a port and the router binding it: callers that route in-process
(bench/run.py) release it when the route ends, so a fast board set does not run out of the 100 ports.
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


def _claims(change) -> int | None:
    """Runs change(claims, now) on the live claims (port -> time) under the file lock and saves the result."""
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
        got = change(claims, now)
        lock.seek(0)
        lock.truncate()
        lock.write(json.dumps(claims))
        return got


def new_port() -> int:
    def claim(claims: dict, now: float) -> int | None:
        for p in range(LO, HI + 1):
            if p not in claims and _free(p):
                claims[p] = now
                return p
        return None
    p = _claims(claim)
    if p is None:
        raise RuntimeError("no free viewer port")
    return p


def release_port(port: int) -> None:
    """The route that used the port has ended (its viewer is closed), so the port may be handed out again."""
    _claims(lambda claims, now: claims.pop(port, None))


def port_of(args: list[str]) -> int:
    return int(args[args.index("--view-port") + 1])


def route_args(embed: bool = True) -> list[str]:
    """embed=False for binaries older than 0.11.0, which reject --view-embed (the stack page then cannot frame them)."""
    a = ["--view", "--view-host", "0.0.0.0", "--view-port", str(new_port()), "--view-token", token(), "--no-freeze"]
    return a + (["--view-embed", embed_origins()] if embed else [])


if __name__ == "__main__":
    print(*route_args(), sep="\n")
