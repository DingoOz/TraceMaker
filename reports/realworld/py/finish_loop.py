#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""finish_loop.py BOARD+planes START_RUN [--max N] [--passes P] [closure options...]: resume START_RUN, then the result, again and again (closure.py --resume), until the board
is complete, a resume gains nothing, or N resumes were made. Runs are named START_RUN-f1, -f2, ... ; each resume is a finishing phase (fine finish and window reroute)."""
import pathlib
import re
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[3]


def main() -> int:
    board_arg, start = sys.argv[1], sys.argv[2]
    rest = sys.argv[3:]
    mx = int(rest[rest.index("--max") + 1]) if "--max" in rest else 4
    passes = rest[rest.index("--passes") + 1] if "--passes" in rest else "2"
    extra = [x for i, x in enumerate(rest) if x not in ("--max", "--passes") and (i == 0 or rest[i - 1] not in ("--max", "--passes"))]
    board = board_arg.removesuffix("+planes")
    prev, best = start, None
    for k in range(1, mx + 1):
        name = f"{start}-f{k}"
        log = ROOT / "reports/realworld/boards" / board / f"{name}.log"
        subprocess.run([sys.executable, str(ROOT / "reports/realworld/py/closure.py"), board_arg, name, "--resume", prev, "--finish-passes", passes] + extra, stdout=open(log, "w"), stderr=subprocess.STDOUT)
        m = re.findall(r"best state (\d+) open", log.read_text())
        n = int(m[-1]) if m else None
        print(f"{board} {name}: {n} open", flush=True)
        if n is None or n == 0 or (best is not None and n >= best):
            break
        best, prev = n, name
    return 0


if __name__ == "__main__":
    sys.exit(main())
