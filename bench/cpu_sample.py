#!/usr/bin/env python3
"""Run a command and sample the CPU use of every thread of it and its child processes (from /proc), for the
parallelism overlay of the comparison videos.

  python3 bench/cpu_sample.py out.json [--every 0.25] [--gpu] -- command args...

out.json: {"cmd", "every", "clk_tck", "samples": [{"t": s, "threads": {"pid/tid name": cpu_fraction}}...]}, where
cpu_fraction is the share of one core the thread used since the previous sample (1.0 = one core fully busy).
With --gpu, "gpu": [{"t", "gpu", "sm"}] holds the SM utilisation (%) of the job's processes from `nvidia-smi pmon`
(1 s resolution; the GPUs are shared, so only rows with the job's pids count).
"""
import threading
import json
import os
import shutil
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
    gpu_rows, pmon = [], None
    if "--gpu" in opts and shutil.which("nvidia-smi"):
        pmon = subprocess.Popen(["nvidia-smi", "pmon", "-s", "u", "-d", "1"], stdout=subprocess.PIPE, text=True)

        def read_pmon():
            for line in pmon.stdout:
                f = line.split()
                if len(f) > 3 and not line.startswith("#") and f[1].isdigit():
                    gpu_rows.append((time.monotonic() - t0, int(f[0]), int(f[1]), f[3]))
        threading.Thread(target=read_pmon, daemon=True).start()
    pids_seen = set()
    prev, prev_t, samples = {}, t0, []
    while True:
        done = proc.poll() is not None
        now = time.monotonic()
        tree = children(proc.pid) if not done else []
        pids_seen.update(tree)
        cur = thread_times(tree)
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
    seconds = round(time.monotonic() - t0, 2)
    if pmon:
        pmon.terminate()
    gpu = [{"t": round(t, 2), "gpu": g, "sm": int(sm) if sm.isdigit() else 0} for t, g, pid, sm in gpu_rows if pid in pids_seen]
    json.dump({"cmd": cmd, "every": every, "clk_tck": CLK, "seconds": seconds, "samples": samples, "gpu": gpu}, open(out, "w"))
    return proc.returncode


if __name__ == "__main__":
    raise SystemExit(main())
