# SPDX-License-Identifier: GPL-3.0-or-later
"""Everything TraceMaker is doing on this machine, for the progress site's live page (/live, D165).

Every tracemaker process writes <dir>/<pid>.json about once a second (src/core/live.hpp) and every bench/run.py run writes
<dir>/run-<pid>.json (bench/live_run.py); <dir> is $TM_LIVE_DIR, else $XDG_CACHE_HOME/tracemaker/live, else
~/.cache/tracemaker/live. This module reads them all, whichever checkout or worktree the process runs from, adds what /proc
says about the processes still alive (CPU use, memory, threads) and puts each board's router under its bench run.
Standard library only; read-only.
"""
from __future__ import annotations

import json
import os
import pathlib
import time

CLK_TCK = os.sysconf("SC_CLK_TCK")
_cpu_prev: dict[int, tuple[float, float]] = {}  # pid -> (time, CPU seconds) of the last poll, for CPU use
_roots: dict[str, tuple[str, str]] = {}          # directory -> (worktree root, branch)


def live_dir() -> pathlib.Path:
    if os.environ.get("TM_LIVE_DIR"):
        return pathlib.Path(os.environ["TM_LIVE_DIR"])
    if os.environ.get("XDG_CACHE_HOME"):
        return pathlib.Path(os.environ["XDG_CACHE_HOME"]) / "tracemaker" / "live"
    return pathlib.Path.home() / ".cache" / "tracemaker" / "live"


def _stat(pid: int) -> list[str] | None:
    """Fields of /proc/<pid>/stat from field 3 on (after the command name), or None if the process is gone."""
    try:
        s = pathlib.Path(f"/proc/{pid}/stat").read_text()
        return s[s.rindex(")") + 2:].split()
    except (OSError, ValueError):
        return None


def _alive(pid: int, start) -> list[str] | None:
    st = _stat(pid)
    if st is None or (start is not None and st[19] != str(start)):  # field 22: a later process given the same pid is not it
        return None
    return st


def _proc(pid: int, st: list[str]) -> dict:
    now = time.time()
    cpu = (int(st[11]) + int(st[12])) / CLK_TCK  # utime, stime
    prev = _cpu_prev.get(pid)
    _cpu_prev[pid] = (now, cpu)
    out = {"threads": int(st[17]), "rss_mb": round(int(st[21]) * os.sysconf("SC_PAGE_SIZE") / 2**20, 1), "cpu_total_s": round(cpu, 1)}
    if prev and now > prev[0]:
        out["cpu_pct"] = round(100 * (cpu - prev[1]) / (now - prev[0]))
    return out


def worktree(path: str) -> tuple[str, str]:
    """The git checkout a path lies in and its branch ("", "") when none."""
    p = pathlib.Path(path)
    key = str(p)
    if key in _roots:
        return _roots[key]
    res = ("", "")
    for d in [p, *p.parents]:
        g = d / ".git"
        if g.exists():
            try:
                gitdir = pathlib.Path(g.read_text().split(":", 1)[1].strip()) if g.is_file() else g
                head = (gitdir / "HEAD").read_text().strip()
                res = (str(d), head.rsplit("/", 1)[-1] if head.startswith("ref:") else head[:8])
            except (OSError, IndexError):
                res = (str(d), "")
            break
    _roots[key] = res
    return res


def _read(f: pathlib.Path) -> dict | None:
    try:
        return json.loads(f.read_text())
    except (OSError, ValueError):
        return None  # being replaced, or a process killed while writing (the rename makes that rare)


def snapshot(keep_s: float = 1800) -> dict:
    """Processes and bench runs that are alive, or ended less than keep_s seconds ago."""
    d = live_dir()
    now = time.time()
    procs, runs = [], []
    for f in sorted(d.glob("*.json")) if d.is_dir() else []:
        j = _read(f)
        if not j or "pid" not in j:
            continue
        st = _alive(j["pid"], j.get("proc_start"))
        if j.get("state") == "running":
            if st is None:
                j["state"] = "killed"  # gone without writing its last state (a signal, or the bench's timeout)
            else:
                j["proc"] = _proc(j["pid"], st)
        if j["state"] != "running" and now - j.get("updated", 0) > keep_s:
            continue
        j["age_s"] = round(now - j.get("updated", now), 1)
        if j.get("kind") == "bench":
            j["worktree"], j["branch"] = worktree(j.get("root", ""))
            runs.append(j)
        else:
            j["worktree"], j["branch"] = worktree(j.get("exe") or j.get("cwd", ""))
            procs.append(j)
    for p in [p for p in _cpu_prev if not any(q["pid"] == p and "proc" in q for q in procs)]:
        _cpu_prev.pop(p, None)
    # A board's router belongs to the bench run that started it (its parent, with the same run name).
    by_run = {(r["pid"], r["run"]): r for r in runs}
    for r in runs:
        r["procs"] = []
    loose = []
    for p in procs:
        r = by_run.get((p.get("ppid"), p.get("run")))
        (r["procs"] if r else loose).append(p)
    order = {"running": 0, "killed": 1, "ended": 1, "done": 2}
    key = lambda x: (order.get(x["state"], 3), -x.get("started", 0))
    runs.sort(key=key)
    loose.sort(key=key)
    for r in runs:
        r["procs"].sort(key=key)
    try:
        load = os.getloadavg()
    except OSError:
        load = (0, 0, 0)
    mem = {}
    try:
        for line in pathlib.Path("/proc/meminfo").read_text().splitlines():
            k, v = line.split(":", 1)
            if k in ("MemTotal", "MemAvailable"):
                mem[k] = int(v.split()[0]) // 1024
    except OSError:
        pass
    return {"generated": now, "dir": str(d), "keep_s": keep_s, "cpus": os.cpu_count(), "load": [round(x, 2) for x in load], "mem_mb": mem,
            "runs": runs, "procs": loose}
