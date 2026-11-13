# SPDX-License-Identifier: GPL-3.0-or-later
"""KiCad 10 IPC action plugin: route the nets of the selected items with TraceMaker, through the session daemon.

The daemon (`tracemaker daemon`, design doc 19 §10) holds a session of the board between actions, so routing a second net reuses the board
read, the rules and the session's caches. Each action:

1. fingerprints the open board (tracks, vias, footprints, zones and the project's rule files);
2. asks the daemon for the session whose current state was recorded with that fingerprint, or starts one from a saved copy of the board;
3. routes the selected nets in the session (every segment and via passes the exact check, and the route is refused if it adds a DRC error);
4. adds the new copper to the open board in ONE commit (one undo), and records the board's new fingerprint for the session's new state.

Any change to the board between actions (an edit, an undo, a refill) changes the fingerprint, and the next action starts a fresh session from
the board as it is. The daemon is started on first use and exits after 30 minutes without a client.

Environment (besides those of tracemaker_route.py: TRACEMAKER):
  TRACEMAKER_NET_WORK     search work for the route (default 2000000)
  TRACEMAKER_SOCKET       daemon socket (default: `tracemaker daemon --print-socket`)
"""
from __future__ import annotations

import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)


class DaemonError(RuntimeError):
    pass


def request(sock_path: str, req: dict, timeout: float = 600.0) -> dict:
    """One JSON-lines request to the daemon."""
    s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    s.settimeout(timeout)
    try:
        s.connect(sock_path)
        s.sendall((json.dumps(req) + "\n").encode())
        buf = b""
        while not buf.endswith(b"\n"):
            chunk = s.recv(1 << 16)
            if not chunk:
                break
            buf += chunk
    finally:
        s.close()
    if not buf:
        raise DaemonError("the daemon closed the connection")
    ans = json.loads(buf)
    if isinstance(ans, dict) and "error" in ans:
        raise DaemonError(f"{ans['error'].get('code')}: {ans['error'].get('message')}")
    return ans


def socket_path(exe: str) -> str:
    if os.environ.get("TRACEMAKER_SOCKET"):
        return os.environ["TRACEMAKER_SOCKET"]
    return subprocess.run([exe, "daemon", "--print-socket"], capture_output=True, text=True, check=True).stdout.strip()


def ensure_daemon(exe: str, sock_path: str, wait_s: float = 10.0) -> None:
    """Starts the daemon when no daemon answers on the socket."""
    try:
        request(sock_path, {"op": "status"}, timeout=5)
        return
    except (OSError, DaemonError):
        pass
    subprocess.Popen([exe, "daemon", "--socket", sock_path], stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                     start_new_session=True)  # outlives this action
    t0 = time.time()
    while time.time() - t0 < wait_s:
        try:
            request(sock_path, {"op": "status"}, timeout=5)
            return
        except (OSError, DaemonError):
            time.sleep(0.1)
    raise DaemonError(f"the daemon did not start on {sock_path}")


def _proto_bytes(item) -> bytes:
    p = getattr(item, "proto", None)
    return p.SerializeToString(deterministic=True) if p is not None else repr(item).encode()


def fingerprint(board, project_dir: str | None = None) -> str:
    """A digest of what the router reads from the open board: copper, footprints, zones (with fills) and the project's rule files on disk.

    Item order does not matter (sorted). KiCad's item ids are included, so the copper this plugin added is recognised by the next action
    only if nothing touched it."""
    parts = []
    for kind, items in (("t", board.get_tracks()), ("v", board.get_vias()), ("f", board.get_footprints()), ("z", board.get_zones())):
        parts.extend(kind.encode() + hashlib.sha256(_proto_bytes(i)).digest() for i in items)
    h = hashlib.sha256()
    for p in sorted(parts):
        h.update(p)
    if project_dir and os.path.isdir(project_dir):
        for name in sorted(os.listdir(project_dir)):
            if name.endswith((".kicad_pro", ".kicad_dru")):
                with open(os.path.join(project_dir, name), "rb") as f:
                    h.update(name.encode() + b"\0" + f.read())
    return h.hexdigest()


def selected_nets(board) -> list[str]:
    """Net names of the selected pads, tracks, vias, zones and footprints (a footprint selects the nets of its pads)."""
    nets = set()
    for it in board.get_selection():
        net = getattr(it, "net", None)
        if net is not None and getattr(net, "name", ""):
            nets.add(net.name)
        definition = getattr(it, "definition", None)
        for pad in getattr(definition, "pads", []) or []:
            if getattr(pad, "net", None) is not None and pad.net.name:
                nets.add(pad.net.name)
    return sorted(nets)


def route_in_daemon(sock_path: str, key: str, fp: str, save_copy, nets: list[str], work: int) -> tuple[dict, dict, int, bool]:
    """Opens (or reuses) the session, routes `nets`, returns (route result, copper diff, session id, reused). `save_copy(dir)` writes the board."""
    try:
        ans = request(sock_path, {"op": "open", "key": key, "fingerprint": fp})
    except DaemonError:
        ans = None  # no session for this board state: start one from a copy
    if ans is None:
        with tempfile.TemporaryDirectory(prefix="tracemaker-") as tmp:
            path = save_copy(tmp)
            ans = request(sock_path, {"op": "open", "key": key, "fingerprint": fp, "board": path})
    sid, before, reused = ans["session"], ans["state"], bool(ans.get("reused"))
    res = request(sock_path, {"op": "call", "session": sid, "tool": "route", "args": {"nets": nets, "work": work}})["result"]
    diff = request(sock_path, {"op": "diff", "session": sid, "from": before}) if res.get("committed") else {"tracks": [], "vias": []}
    return res, diff, sid, reused


def main() -> int:
    from kipy import KiCad

    from tracemaker_route import build_items, find_binary

    exe = find_binary()
    if not exe:
        print("TraceMaker: no tracemaker binary found (set TRACEMAKER)", file=sys.stderr)
        return 1
    kicad = KiCad()
    board = kicad.get_board()
    nets = selected_nets(board)
    if not nets:
        print("TraceMaker: select a pad, track, footprint or zone of the nets to route")
        return 0
    sock_path = socket_path(exe)
    ensure_daemon(exe, sock_path)
    try:
        project_dir = board.get_project().path
    except Exception:  # noqa: BLE001 - a board without a project: the rules come from the board alone
        project_dir = None
    key = os.path.join(project_dir or "", board.name)
    fp = fingerprint(board, project_dir)

    def save_copy(tmp: str) -> str:
        path = os.path.join(tmp, "board.kicad_pcb")
        board.save_as(path, overwrite=True, include_project=True)
        return path

    t0 = time.time()
    res, diff, sid, reused = route_in_daemon(sock_path, key, fp, save_copy, nets, int(os.environ.get("TRACEMAKER_NET_WORK", "2000000")))
    if not res.get("committed"):
        print(f"TraceMaker: {', '.join(nets)}: nothing added ({res.get('reason', 'no route')}); {res.get('open_after', '?')} connections left open "
              f"({time.time() - t0:.1f} s, {'warm' if reused else 'new'} session)")
        request(sock_path, {"op": "mark", "session": sid, "fingerprint": fp})  # the board is the session's state: keep it warm
        return 0
    new = build_items(board, diff)
    commit = board.begin_commit()
    board.create_items(new)
    board.push_commit(commit, "TraceMaker: route " + ", ".join(nets)[:60])
    # The board now holds the session's state: record that, so the next action finds this session warm.
    request(sock_path, {"op": "mark", "session": sid, "fingerprint": fingerprint(board, project_dir)})
    print(f"TraceMaker: {', '.join(nets)}: added {len(diff['tracks'])} tracks and {len(diff['vias'])} vias, "
          f"{res.get('routed', 0)}/{res.get('connections', 0)} routed, {res.get('open_after', 0)} still open ({time.time() - t0:.1f} s, "
          f"{'warm' if reused else 'new'} session)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
