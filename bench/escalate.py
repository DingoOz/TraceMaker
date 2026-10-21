#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Continues the boards the stack runs left open with more time, resuming from the best saved search state (bench/ledger.py lists them).

For each set in order, while it has open boards and the time schedule has a stage left, run bench/run.py on the open boards as run
stack-<set>-s<N>[-g<K>] with the stage's time per board. Boards that are fully routed but not clean (an added error, or the report cap) are
not continued: time does not help them. Goal first: 100 % routed; speed second. Re-running picks up from the ledger.

  bench/escalate.py [--sets srj19,srj20,tierD,tierC,tierB,tierA] [--dry]
"""
import argparse, collections, pathlib, subprocess, sys, time

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "bench"))
import ledger  # noqa: E402

SRJ = {"time": {2: 3600, 3: 7200, 4: 14400, 5: 28800}, "jobs": 3, "threads": 16, "args": "--relax --variants 16 --no-gpu --fast-bends"}
TIER = {"time": {1: 3600, 2: 7200, 3: 14400}, "jobs": 6, "threads": 8, "args": ""}


def busy():
    """Boards of runs still going (no summary.json yet) that were started from a list in /tmp/claude-1000/<run>.txt: not started a second time."""
    out = set()
    for d in ledger.RES.glob("stack-*"):
        lst = pathlib.Path(f"/tmp/claude-1000/{d.name}.txt")
        if d.is_dir() and not (d / "summary.json").exists() and lst.exists():
            out |= {l.strip() for l in lst.read_text().splitlines() if l.strip() and not l.startswith("#")}
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sets", default="srj19,srj20,tierD,tierC,tierB,tierA")
    ap.add_argument("--dry", action="store_true")
    ap.add_argument("--jobs", type=int, help="boards at once (default: the set's schedule)")
    ap.add_argument("--threads", type=int, help="router threads per board (default: the set's schedule)")
    a = ap.parse_args()
    for key in a.sets.split(","):
        cfg = SRJ if key.startswith("srj") else TIER
        while True:
            led = ledger.build()
            es = [e for e in led.values() if e["set"] == key]
            skip = busy()
            pend = [e for e in es if e["state"] == "open" and e["board"] not in skip]
            stage = max((x["stage"] for e in es for x in e["attempts"]), default=-1) + 1
            if key.startswith("srj"):
                stage = max(stage, 2)
            if not pend or stage not in cfg["time"]:
                print(f"{key}: {len(pend)} open, next stage {stage}: {'nothing to do' if not pend else 'schedule exhausted'}", flush=True)
                break
            groups = collections.defaultdict(list)
            for e in pend:
                b = e["best"]
                groups[b["run"] if b["state"] else ""].append(e["board"])
            for g, (resume, boards) in enumerate(sorted(groups.items())):
                name = f"stack-{key}-s{stage}-g{g}"
                lst = pathlib.Path(f"/tmp/claude-1000/{name}.txt")
                lst.write_text("\n".join(sorted(boards)) + "\n")
                cmd = ["python3", "bench/run.py", "--board-list", str(lst), "--set-name", key, "--time", str(cfg["time"][stage]), "--jobs", str(a.jobs or cfg["jobs"]),
                       "--threads", str(a.threads or cfg["threads"]), "--name", name, "--view"]
                if key.startswith("srj"):
                    cmd += ["--fixtures", f"bench/data/tscircuit/boards/{key}"]
                else:
                    cmd += ["--tier", key[-1]]
                if resume:
                    cmd += ["--resume-run", resume]
                if cfg["args"]:
                    cmd += [f"--route-args={cfg['args']}"]
                print(time.strftime("%H:%M:%S"), name, len(boards), "boards,", cfg["time"][stage], "s each, resume", resume or "-", flush=True)
                if a.dry:
                    print(" ", " ".join(cmd))
                    continue
                with open(ROOT / f"bench/results/{name}.log", "w") as lf:
                    subprocess.run(cmd, cwd=ROOT, stdout=lf, stderr=subprocess.STDOUT)
                subprocess.run(["python3", "bench/ledger.py"], cwd=ROOT, capture_output=True)
            if a.dry:
                break
    print("escalation finished", flush=True)


if __name__ == "__main__":
    main()
