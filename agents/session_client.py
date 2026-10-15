#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""A small client for `tracemaker session --jsonl` (doc 19): start the session process, call its tools, stop it.

Standard library only. An agent written in Python uses it; an agent that speaks MCP does not need it (`tracemaker session BOARD` serves MCP).

    with Session(tm, board, out, log="session.log", work_limit=50_000_000) as s:
        info = s.call("board_info")
        s.call("route", work=2_000_000, variants=4)
        s.call("save")
"""
import json
import subprocess


class SessionError(RuntimeError):
    def __init__(self, tool, error):
        super().__init__(f"{tool}: {error['code']}: {error['message']}")
        self.tool, self.code, self.message = tool, error["code"], error["message"]


class Session:
    def __init__(self, tracemaker, board, out, log=None, work_limit=0, threads=4, seed=1, extra=()):
        cmd = [str(tracemaker), "session", str(board), "--jsonl", "--out", str(out), "--threads", str(threads), "--seed", str(seed)]
        if log:
            cmd += ["--log", str(log)]
        if work_limit:
            cmd += ["--work-limit", str(int(work_limit))]
        self._p = subprocess.Popen(cmd + list(extra), stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, text=True, bufsize=1)

    def call(self, tool, raise_errors=True, **args):
        """One tool call. A refused call raises SessionError, or, with raise_errors=False, comes back as {"error": ...}."""
        self._p.stdin.write(json.dumps({"tool": tool, "args": args}) + "\n")
        self._p.stdin.flush()
        line = self._p.stdout.readline()
        if not line:
            raise RuntimeError(f"the session ended during {tool}")
        result = json.loads(line)
        if raise_errors and "error" in result:
            raise SessionError(tool, result["error"])
        return result

    def close(self):
        if self._p.poll() is None:
            self._p.stdin.close()
            self._p.wait(timeout=60)

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()
