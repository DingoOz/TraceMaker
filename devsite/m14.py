#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live status page for the M14 BGA-breakout work (docs/16-bga-breakout-plan.md), in the style of the progress site.

Reads dev/m14-status.json (targets, tasks, findings: edited by hand), bench/results/*/ (runs of the srj sets,
including runs still going), running processes, git and nvidia-smi. Read-only; standard library only.

Usage: devsite/m14.py [--host 0.0.0.0] [--port 8767]
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
STATIC = pathlib.Path(__file__).resolve().parent / "static"
RESULTS = ROOT / "bench" / "results"
RUN_RE = re.compile(r"^(gap19|gap20|srj19|srj20|fan31|pf19|pf20)")
SET_OF = {"gap19": "srj19-gap", "gap20": "srj20-gap", "srj19": "srj19", "srj20": "srj20", "fan31": "fanout31", "pf19": "srj19-p1fail", "pf20": "srj20-p1fail"}
_cache: dict[str, tuple[float, object]] = {}
_lock = threading.Lock()


def cached(key: str, ttl: float, fn):
    now = time.time()
    with _lock:
        hit = _cache.get(key)
        if hit and now - hit[0] < ttl:
            return hit[1]
    v = fn()
    with _lock:
        _cache[key] = (now, v)
    return v


def sh(cmd: list[str], timeout: float = 8.0) -> str:
    try:
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def read_json(p: pathlib.Path, default):
    try:
        return json.loads(p.read_text())
    except (OSError, ValueError):
        return default


def set_size(name: str) -> int:
    f = ROOT / "bench" / "sets" / f"{name}.txt"
    try:
        return sum(1 for l in f.read_text().splitlines() if l.strip() and not l.startswith("#"))
    except OSError:
        return 0


def runs(hours: float = 36) -> list[dict]:
    out = []
    now = time.time()
    for d in RESULTS.iterdir() if RESULTS.exists() else []:
        m = RUN_RE.match(d.name)
        if not m or not d.is_dir():
            continue
        bj = d / "boards.jsonl"
        summ = read_json(d / "summary.json", None)
        try:
            mt = max((bj.stat().st_mtime if bj.exists() else 0), (d / "summary.json").stat().st_mtime if summ else 0)
            if not mt:
                mt = d.stat().st_mtime
        except OSError:
            continue
        if now - mt > hours * 3600:
            continue
        rows = []
        if bj.exists():
            for line in bj.read_text().splitlines():
                try:
                    rows.append(json.loads(line))
                except ValueError:
                    pass
        set_name = (summ or {}).get("set") or SET_OF.get(m.group(1), "")
        total = (summ or {}).get("boards") or set_size(set_name)
        n = len(rows)
        clean = sum(1 for r in rows if r.get("clean"))
        routed = sum(r.get("routed", 0) for r in rows)
        conns = sum(r.get("connections", 0) for r in rows)
        secs = sorted(r.get("seconds", 0) for r in rows)
        out.append({
            "run": d.name, "set": set_name, "done": summ is not None, "boards": n, "total": total, "clean": clean,
            "clean_pct": clean / n if n else 0, "routed": routed, "connections": conns,
            "completion": routed / conns if conns else 0, "vias": (sum(r.get("vias", 0) for r in rows) / n) if n else 0,
            "median_s": secs[n // 2] if n else 0, "work": (summ or {}).get("work"),
            "args": (summ or {}).get("route_args", ""), "mtime": mt,
        })
    out.sort(key=lambda r: -r["mtime"])
    return out


def running() -> dict:
    ps = sh(["ps", "-eo", "pid,etimes,args"])
    routes = [l for l in ps.splitlines() if "tracemaker route" in l and "bench/data" in l]
    benches = [l.split(None, 2)[2][:160] for l in ps.splitlines() if "bench/run.py" in l and "python" in l]
    return {"routes": len(routes), "benches": benches}


def gpus() -> list[dict]:
    txt = sh(["nvidia-smi", "--query-gpu=name,utilization.gpu,memory.used,memory.total", "--format=csv,noheader,nounits"])
    res = []
    for l in txt.splitlines():
        p = [x.strip() for x in l.split(",")]
        if len(p) == 4:
            res.append({"name": p[0], "util": float(p[1]), "mem_used": float(p[2]), "mem_total": float(p[3])})
    return res


def git() -> dict:
    branch = sh(["git", "rev-parse", "--abbrev-ref", "HEAD"]).strip()
    log = sh(["git", "log", "-12", "--format=%h\t%s\t%ct"])
    commits = [dict(zip(("hash", "subject", "ts"), l.split("\t"))) for l in log.splitlines() if l.count("\t") == 2]
    dirty = len([l for l in sh(["git", "status", "--short", "src", "bench", "docs", "devsite"]).splitlines() if l.strip()])
    return {"branch": branch, "commits": commits, "dirty": dirty}


def activity() -> list[dict]:
    p = ROOT / "dev" / "m14-activity.jsonl"
    try:
        lines = p.read_text().splitlines()[-40:]
    except OSError:
        return []
    out = []
    for l in lines:
        try:
            out.append(json.loads(l))
        except ValueError:
            pass
    return out[::-1]


def status() -> dict:
    load = sh(["cat", "/proc/loadavg"]).split()[:3]
    return {
        "generated": time.time(),
        "plan": read_json(ROOT / "dev" / "m14-status.json", {}),
        "runs": cached("runs", 3, runs),
        "running": cached("running", 2, running),
        "gpus": cached("gpus", 4, gpus),
        "git": cached("git", 5, git),
        "activity": activity(),
        "load": load,
    }


PAGE = None


def page() -> bytes:
    global PAGE
    html = (STATIC / "index.html").read_text()
    css = html[html.index("<style>") + 7:html.index("</style>")]
    tpl = (STATIC / "m14.html").read_text()
    return tpl.replace("/*BASE_CSS*/", css).encode()


class Handler(BaseHTTPRequestHandler):
    server_version = "TraceMakerM14/0.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/api/status":
            self._send(200, json.dumps(status()).encode(), "application/json")
        elif path in ("/", "/index.html"):
            self._send(200, page(), "text/html; charset=utf-8")
        else:
            self._send(404, b"not found", "text/plain")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8767)
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"M14 status page on http://{a.host}:{a.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
