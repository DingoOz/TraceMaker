# SPDX-License-Identifier: GPL-3.0-or-later
"""A bench run's state for the progress site's live page (devsite /live, D165).

bench/run.py writes <dir>/run-<pid>.json whenever a board starts, changes step or finishes, in the directory the engine
writes its own <pid>.json files to (src/core/live.hpp): $TM_LIVE_DIR, else $XDG_CACHE_HOME/tracemaker/live, else
~/.cache/tracemaker/live. Each tracemaker process the run starts gets TM_LIVE_RUN and TM_LIVE_BOARD in its environment, so the
page can put a board's router under its run. TM_LIVE=0 turns it off, as for the engine. Nothing reads it back.
"""
from __future__ import annotations

import atexit
import json
import os
import pathlib
import threading
import time


def live_dir() -> pathlib.Path | None:
    if os.environ.get("TM_LIVE") == "0":
        return None
    if os.environ.get("TM_LIVE_DIR"):
        return pathlib.Path(os.environ["TM_LIVE_DIR"])
    if os.environ.get("XDG_CACHE_HOME"):
        return pathlib.Path(os.environ["XDG_CACHE_HOME"]) / "tracemaker" / "live"
    return pathlib.Path.home() / ".cache" / "tracemaker" / "live"


def proc_start() -> int | None:
    """Field 22 of /proc/self/stat, so the page can tell this process from a later one given the same pid."""
    try:
        s = pathlib.Path("/proc/self/stat").read_text()
        return int(s[s.rindex(")") + 2:].split()[19])
    except (OSError, ValueError, IndexError):
        return None


class LiveRun:
    def __init__(self, run: str, info: dict, boards: list[str]) -> None:
        d = live_dir()
        self.path = None
        if d is not None:
            try:
                d.mkdir(parents=True, exist_ok=True)
                self.path = d / f"run-{os.getpid()}.json"
            except OSError:
                self.path = None
        self.lock = threading.Lock()
        self.state = {"kind": "bench", "run": run, "pid": os.getpid(), "proc_start": proc_start(), "started": time.time(),
                      "state": "running", "boards": boards, "running": {}, "finished": [], **info}
        self._write()
        atexit.register(self.end)

    def env(self, board: str) -> dict:
        """The environment of a tracemaker process routing `board`."""
        return {**os.environ, "TM_LIVE_RUN": self.state["run"], "TM_LIVE_BOARD": board}

    def step(self, board: str, what: str) -> None:
        with self.lock:
            r = self.state["running"].setdefault(board, {"since": time.time()})
            r["step"], r["step_since"] = what, time.time()
            self._write()

    def finished(self, row: dict) -> None:
        keep = ("board", "routed", "connections", "clean", "completion", "seconds", "cpu_s", "wall_s", "unconnected_after", "added_errors", "error", "exit")
        with self.lock:
            self.state["running"].pop(row.get("board"), None)
            self.state["finished"].append({k: row[k] for k in keep if k in row})
            self._write()

    def end(self, summary: dict | None = None) -> None:
        with self.lock:
            if self.state["state"] != "running":
                return
            self.state["state"] = "done"
            if summary:
                self.state["summary"] = {k: summary.get(k) for k in ("clean_pass", "completion", "boards", "cpu_seconds")}
            self._write()

    def _write(self) -> None:
        if self.path is None:
            return
        self.state["updated"] = time.time()
        tmp = self.path.with_suffix(".json.tmp")
        try:
            tmp.write_text(json.dumps(self.state))
            os.replace(tmp, self.path)
        except OSError:
            pass
