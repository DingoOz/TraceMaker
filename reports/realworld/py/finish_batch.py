#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""finish_batch.py LIST RUN OUT_RUN [--jobs N]: for each `BOARD OPEN LAYERS` line of LIST, finish_loop.py from BOARD/RUN (repeated finishing phases), then the ledger and the safety
simulation of the best run it made when that run has no open connection; rows go to batch/OUT_RUN.jsonl (same fields as batch.py)."""
import concurrent.futures as cf
import json
import pathlib
import re
import subprocess
import sys

RW = pathlib.Path(__file__).resolve().parents[1]


def one(board: str, start: str, layers: str, out_run: str) -> dict:
    row = {"board": board, "run": out_run, "layers": int(layers), "from": start}
    subprocess.run([sys.executable, str(RW / "py/finish_loop.py"), f"{board}+planes", start, "--max", "3", "--passes", "3", "--threads", "4", "--trial-jobs", "2", "--no-fine", "--work", "20000000"],
                   stdout=open(RW / "boards" / board / f"{out_run}.log", "w"), stderr=subprocess.STDOUT)
    best = (None, 10 ** 9)
    for lg in sorted((RW / "boards" / board).glob(f"{start}-f*.log")):
        m = re.findall(r"best state (\d+) open", lg.read_text())
        if m and int(m[-1]) < best[1]:
            best = (lg.name[:-4], int(m[-1]))
    if best[0] is None:
        best = (start, int(json.loads((RW / "boards" / board / start / "closure.json").read_text())["rounds"][-1].get("open", 0)))
    row["best_run"], row["open_best"] = best
    if best[1] == 0:
        subprocess.run(["bash", str(RW / "py/judge_run.sh"), board, best[0], layers], stdout=open(RW / "boards" / board / f"{best[0]}.judge.log", "w"), stderr=subprocess.STDOUT)
        lj = RW / "boards" / board / best[0] / "ledger.json"
        sj = RW / "sim/results" / board / f"safety.{best[0]}.json"
        if lj.exists():
            led = json.loads(lj.read_text())
            row["kicad_open"], row["compromises"] = led["unconnected"], len(led["added"])
            row["shorts"] = sum(1 for a in led["added"] if a["type"] == "shorting_items" and any(i.startswith(("Track", "Via", "Arc")) for i in a.get("items", [])))
        if sj.exists():
            row["safety"] = json.loads(sj.read_text()).get("verdict")
    return row


def main() -> int:
    lst, start, out_run = sys.argv[1], sys.argv[2], sys.argv[3]
    jobs = int(sys.argv[sys.argv.index("--jobs") + 1]) if "--jobs" in sys.argv else 2
    items = [l.split() for l in pathlib.Path(lst).read_text().splitlines() if l.strip()]
    with cf.ThreadPoolExecutor(jobs) as ex, open(RW / "batch" / f"{out_run}.jsonl", "a") as fh:
        for f in cf.as_completed([ex.submit(one, b, start, l, out_run) for b, _, l in items]):
            r = f.result()
            fh.write(json.dumps(r) + "\n")
            fh.flush()
            print(r, flush=True)
    return 0


if __name__ == "__main__":
    sys.exit(main())
