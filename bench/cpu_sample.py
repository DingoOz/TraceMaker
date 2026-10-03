#!/usr/bin/env python3
"""Run a command and sample the CPU use of every thread of it and its child processes (from /proc), for the
parallelism overlay of the comparison videos.

  python3 bench/cpu_sample.py out.json [--every 0.25] -- command args...

out.json: {"cmd", "every", "clk_tck", "samples": [{"t": s, "threads": {"pid/tid name": cpu_fraction}}...]}, where
cpu_fraction is the share of one core the thread used since the previous sample (1.0 = one core fully busy).
"""
import json
import os
import subprocess
import sys
import time

CLK = os.sysconf("SC_CLK_TCK")


def children(pid: int) -> list[int]:
    out, todo = [], [pid]
    while todo:
        p = todo.pop()
        out.append(p)
        try:
            for t in os.listdir(f"/proc/{p}/task"):
                with open(f"/proc/{p}/task/{t}/children") as f:
                    todo.extend(int(c) for c in f.read().split())
        except OSError:
            pass
    return out


def thread_times(pids: list[int]) -> dict:
    out = {}
    for p in pids:
        try:
            for t in os.listdir(f"/proc/{p}/task"):
                with open(f"/proc/{p}/task/{t}/stat") as f:
                    s = f.read()
                name = s[s.find("(") + 1:s.rfind(")")]
                f_ = s[s.rfind(")") + 2:].split()
                out[f"{p}/{t} {name}"] = int(f_[11]) + int(f_[12])  # utime + stime, clock ticks
        except OSError:
            pass
    return out


def main() -> int:
    args = sys.argv[1:]
    if "--" not in args:
        print(__doc__)
        return 2
    sep = args.index("--")
    opts, cmd = args[:sep], args[sep + 1:]
    out = opts[0]
    every = float(opts[opts.index("--every") + 1]) if "--every" in opts else 0.25
    t0 = time.monotonic()
    proc = subprocess.Popen(cmd)
    prev, prev_t, samples = {}, t0, []
    while True:
        done = proc.poll() is not None
        now = time.monotonic()
        cur = thread_times(children(proc.pid)) if not done else {}
        dt = now - prev_t
        if dt > 0 and cur:
            samples.append({"t": round(now - t0, 3),
                            # a thread first seen now used all its time in this interval
                            "threads": {k: round((v - prev.get(k, 0)) / CLK / dt, 3) for k, v in cur.items()} if prev else
                                       {k: 0.0 for k in cur}})
        prev, prev_t = cur, now
        if done:
            break
        time.sleep(every)
    json.dump({"cmd": cmd, "every": every, "clk_tck": CLK, "seconds": round(time.monotonic() - t0, 2), "samples": samples},
              open(out, "w"))
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
