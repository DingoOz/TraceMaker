#!/usr/bin/env python3
"""TraceMaker development progress website.

Read-only dashboard over the repository: roadmap progress (dev/progress.json), activity log
(dev/activity.jsonl), test results (build/*/test-results/*.xml), code size, git history, GPU memory and
benchmark summaries. Standard library only.

Usage: devsite/server.py [--host 0.0.0.0] [--port 8765]
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import subprocess
import threading
import time
import xml.etree.ElementTree as ET
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
STATIC = pathlib.Path(__file__).resolve().parent / "static"
CODE_DIRS = ["src", "tests", "bench", "scripts", "devsite", "viewer", "kicad_plugin", "bindings"]
CODE_EXT = {".cpp": "C++", ".hpp": "C++", ".h": "C++", ".cu": "CUDA", ".cuh": "CUDA", ".py": "Python",
            ".ts": "TypeScript", ".js": "JavaScript", ".html": "HTML", ".css": "CSS", ".fbs": "FlatBuffers",
            ".cmake": "CMake", ".sh": "Shell", ".wgsl": "Shaders", ".glsl": "Shaders"}
SKIP_DIRS = {"build", "node_modules", "__pycache__", ".git", "data", "results", "dist"}


def run(cmd: list[str], timeout: float = 10.0) -> str:
    try:
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


class Cache:
    """Caches slow probes (nvidia-smi, kicad-cli, file walks) for a few seconds."""

    def __init__(self) -> None:
        self._lock = threading.Lock()
        self._data: dict[str, tuple[float, object]] = {}

    def get(self, key: str, ttl: float, fn):
        now = time.time()
        with self._lock:
            hit = self._data.get(key)
            if hit and now - hit[0] < ttl:
                return hit[1]
        value = fn()
        with self._lock:
            self._data[key] = (now, value)
        return value


CACHE = Cache()


def read_json(path: pathlib.Path, default):
    try:
        return json.loads(path.read_text())
    except (OSError, ValueError):
        return default


def activity(limit: int = 60) -> list[dict]:
    path = ROOT / "dev" / "activity.jsonl"
    try:
        lines = path.read_text().splitlines()
    except OSError:
        return []
    out = []
    for line in lines[-limit:]:
        try:
            out.append(json.loads(line))
        except ValueError:
            pass
    return list(reversed(out))


def tests() -> list[dict]:
    """One entry per build preset with JUnit results."""
    presets = []
    for d in sorted((ROOT / "build").glob("*/test-results")):
        cases = []
        newest = 0.0
        for f in sorted(d.glob("*.xml")):
            newest = max(newest, f.stat().st_mtime)
            try:
                tree = ET.parse(f)
            except ET.ParseError:
                continue
            for tc in tree.iter("testcase"):
                name = tc.get("name", "?")
                if tc.find("failure") is not None or tc.find("error") is not None:
                    status = "fail"
                elif tc.find("skipped") is not None:
                    status = "skip"
                else:
                    status = "pass"
                cases.append({"name": name, "status": status, "time": float(tc.get("time", "0") or 0)})
        # Catch2 writes one testcase per section; collapse to one row per test name, worst status wins.
        worst = {"fail": 2, "skip": 1, "pass": 0}
        merged: dict[str, dict] = {}
        for c in cases:
            m = merged.setdefault(c["name"], {"name": c["name"], "status": "pass", "time": 0.0})
            m["time"] += c["time"]
            if worst[c["status"]] > worst[m["status"]]:
                m["status"] = c["status"]
        rows = sorted(merged.values(), key=lambda r: (-worst[r["status"]], r["name"]))
        presets.append({"preset": d.parent.name, "updated": newest, "cases": rows,
                        "pass": sum(r["status"] == "pass" for r in rows),
                        "fail": sum(r["status"] == "fail" for r in rows),
                        "skip": sum(r["status"] == "skip" for r in rows)})
    return presets


def builds() -> list[dict]:
    out = []
    for d in sorted((ROOT / "build").glob("*/CMakeCache.txt")):
        b = d.parent
        exe = b / "src" / "app" / "tracemaker"
        out.append({"preset": b.name, "configured": d.stat().st_mtime,
                    "binary": exe.stat().st_mtime if exe.exists() else None})
    return out


def code_stats() -> dict:
    by_lang: dict[str, int] = {}
    by_module: dict[str, int] = {}
    files = 0
    for top in CODE_DIRS:
        base = ROOT / top
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [x for x in dirnames if x not in SKIP_DIRS]
            for fn in filenames:
                p = pathlib.Path(dirpath, fn)
                lang = CODE_EXT.get(p.suffix) or ("CMake" if fn == "CMakeLists.txt" else None)
                if not lang:
                    continue
                try:
                    n = sum(1 for line in p.open(errors="ignore") if line.strip())
                except OSError:
                    continue
                files += 1
                by_lang[lang] = by_lang.get(lang, 0) + n
                rel = p.relative_to(ROOT).parts
                module = "/".join(rel[:2]) if rel[0] == "src" and len(rel) > 2 else rel[0]
                by_module[module] = by_module.get(module, 0) + n
    return {"files": files, "lines": sum(by_lang.values()),
            "by_language": sorted(by_lang.items(), key=lambda kv: -kv[1]),
            "by_module": sorted(by_module.items(), key=lambda kv: -kv[1])}


def git() -> dict:
    log = run(["git", "log", "-n", "25", "--date=iso-strict", "--pretty=format:%h%x1f%ad%x1f%s"])
    commits = []
    for line in log.splitlines():
        parts = line.split("\x1f")
        if len(parts) == 3:
            commits.append({"hash": parts[0], "date": parts[1], "subject": parts[2]})
    count = run(["git", "rev-list", "--count", "HEAD"]).strip()
    status = run(["git", "status", "--porcelain"]).splitlines()
    return {"branch": run(["git", "branch", "--show-current"]).strip(), "commits": commits,
            "commit_count": int(count) if count.isdigit() else 0, "uncommitted_files": len(status)}


def gpus() -> list[dict]:
    q = run(["nvidia-smi", "--query-gpu=index,name,uuid,memory.used,memory.total,utilization.gpu,temperature.gpu",
             "--format=csv,noheader,nounits"], timeout=5)
    out = []
    for line in q.splitlines():
        f = [x.strip() for x in line.split(",")]
        if len(f) == 7:
            out.append({"index": int(f[0]), "name": f[1], "uuid": f[2], "used_mib": int(f[3]),
                        "total_mib": int(f[4]), "util": int(f[5]), "temp": int(f[6])})
    return out


def kicad_version() -> str:
    lines = run(["kicad-cli", "version"], timeout=60).strip().splitlines()
    return lines[-1] if lines else "not found"


def docs() -> list[dict]:
    out = []
    for p in [ROOT / "PLAN.md", *sorted((ROOT / "docs").glob("*.md"))]:
        if not p.exists():
            continue
        title = p.stem
        for line in p.read_text(errors="ignore").splitlines():
            if line.startswith("# "):
                title = line[2:].strip()
                break
        out.append({"path": str(p.relative_to(ROOT)), "title": title, "updated": p.stat().st_mtime})
    return out


def benchmarks() -> list[dict]:
    """Full benchmark runs (>= 20 boards), oldest first (the page shows newest first)."""
    out = []
    files = sorted((ROOT / "bench" / "results").glob("*/summary.json"), key=lambda f: f.stat().st_mtime)
    for f in files:
        s = read_json(f, None)
        if isinstance(s, dict) and (s.get("boards") or 0) >= 20:
            s.setdefault("run", f.parent.name)
            s["finished"] = f.stat().st_mtime
            out.append(s)
    return out[-30:]


HL_LANG = {".cpp": "cpp", ".hpp": "cpp", ".h": "cpp", ".cu": "cpp", ".cuh": "cpp", ".py": "python", ".ts": "typescript",
           ".js": "javascript", ".html": "xml", ".css": "css", ".cmake": "cmake", ".sh": "bash", ".json": "json",
           ".md": "markdown", ".fbs": "cpp", ".wgsl": "rust", ".glsl": "glsl"}
LANG_LABEL = {"cpp": "C++", "python": "Python", "typescript": "TypeScript", "javascript": "JavaScript", "xml": "HTML",
              "css": "CSS", "cmake": "CMake", "bash": "Shell", "json": "JSON", "markdown": "Markdown", "rust": "WGSL",
              "glsl": "GLSL"}


def latest_code(max_lines: int = 80) -> dict | None:
    """The most recently written section of code: the newest changed hunks of the most recently modified file."""
    newest: tuple[float, pathlib.Path] | None = None
    for top in CODE_DIRS:
        base = ROOT / top
        if not base.is_dir():
            continue
        for dirpath, dirnames, filenames in os.walk(base):
            dirnames[:] = [x for x in dirnames if x not in SKIP_DIRS]
            for fn in filenames:
                pth = pathlib.Path(dirpath, fn)
                if pth.suffix not in HL_LANG and fn != "CMakeLists.txt":
                    continue
                try:
                    m = pth.stat().st_mtime
                except OSError:
                    continue
                if newest is None or m > newest[0]:
                    newest = (m, pth)
    if newest is None:
        return None
    mtime, path = newest
    rel = str(path.relative_to(ROOT))
    lang = "cmake" if path.name == "CMakeLists.txt" else HL_LANG[path.suffix]
    try:
        lines = path.read_text(errors="replace").splitlines()
    except OSError:
        return None
    # Added/changed lines versus the last commit (new files: the whole file is new).
    tracked = run(["git", "ls-files", "--error-unmatch", rel]).strip() != ""
    added: set[int] = set()
    if tracked:
        diff = run(["git", "diff", "-U0", "HEAD", "--", rel])
        for line in diff.splitlines():
            if line.startswith("@@"):
                try:
                    plus = line.split("+", 1)[1].split(" ", 1)[0]
                    start, _, count = plus.partition(",")
                    n = int(count) if count else 1
                    added.update(range(int(start), int(start) + n))
                except (ValueError, IndexError):
                    pass
    else:
        added = set(range(1, len(lines) + 1))
    if added:
        last = max(added)
        end = min(len(lines), last + 3)
        begin = max(1, end - max_lines + 1)
        # Prefer to start at the beginning of the newest contiguous changed block when it fits.
        block_start = last
        while block_start - 1 in added:
            block_start -= 1
        if end - block_start + 4 <= max_lines:
            begin = max(1, block_start - 3)
            end = min(len(lines), begin + max_lines - 1)
    else:
        end = len(lines)
        begin = max(1, end - max_lines + 1)
    return {"path": rel, "language": lang, "label": LANG_LABEL.get(lang, lang), "mtime": mtime,
            "start": begin, "lines": lines[begin - 1:end], "added": sorted(i for i in added if begin <= i <= end),
            "total_lines": len(lines), "new_file": not tracked}


def status() -> dict:
    return {
        "generated": time.time(),
        "progress": read_json(ROOT / "dev" / "progress.json", {"milestones": []}),
        "activity": activity(),
        "tests": CACHE.get("tests", 3, tests),
        "builds": CACHE.get("builds", 3, builds),
        "code": CACHE.get("code", 10, code_stats),
        "git": CACHE.get("git", 5, git),
        "gpus": CACHE.get("gpus", 5, gpus),
        "kicad": CACHE.get("kicad", 3600, kicad_version),
        "docs": CACHE.get("docs", 10, docs),
        "benchmarks": CACHE.get("bench", 10, benchmarks),
        "latest_code": CACHE.get("latest_code", 3, latest_code),
    }


class Handler(BaseHTTPRequestHandler):
    server_version = "TraceMakerDev/0.1"

    def log_message(self, fmt, *args):  # quiet
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/api/status":
            self._send(200, json.dumps(status()).encode(), "application/json")
        elif path in ("/", "/index.html"):
            self._send(200, (STATIC / "index.html").read_bytes(), "text/html; charset=utf-8")
        else:
            self._send(404, b"not found", "text/plain")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8765)
    a = ap.parse_args()
    threading.Thread(target=lambda: CACHE.get("kicad", 3600, kicad_version), daemon=True).start()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"TraceMaker progress site on http://{a.host}:{a.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
