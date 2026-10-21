#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Counts undocumented declarations per header and compares with the committed baseline (D134).

  scripts/check_api_docs.py [--doxygen doxygen] [--update] [--strict]

Runs Doxygen with docs/api/Doxyfile but EXTRACT_ALL = NO and WARN_IF_UNDOCUMENTED = YES (through the comment filter,
so a plain `//` comment above a declaration counts as documentation), then counts the "is not documented" warnings
per header. A header with more than in docs/api/undocumented-baseline.txt gets a warning (a GitHub annotation in CI);
--strict makes that an error. Fewer is good: --update lowers the baseline. Old code is not required to catch up; new
declarations are expected to carry a comment.
"""
import argparse
import collections
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BASELINE = os.path.join(ROOT, "docs", "api", "undocumented-baseline.txt")
OVERRIDES = ["EXTRACT_ALL = NO", "WARN_IF_UNDOCUMENTED = YES", "SOURCE_BROWSER = NO", "SEARCHENGINE = NO",
             "OUTPUT_DIRECTORY = build/api-check", "WARN_LOGFILE = build/api-check/warnings.log"]


def count(doxygen: str) -> dict[str, int]:
    os.makedirs(os.path.join(ROOT, "build", "api-check"), exist_ok=True)
    with open(os.path.join(ROOT, "docs", "api", "Doxyfile")) as f:
        config = f.read() + "\n" + "\n".join(OVERRIDES) + "\n"
    subprocess.run([doxygen, "-"], input=config, text=True, cwd=ROOT, check=True)
    counts: collections.Counter[str] = collections.Counter()
    with open(os.path.join(ROOT, "build", "api-check", "warnings.log")) as f:
        for line in f:
            m = re.match(r"^(.+?):\d+: warning: .* is not documented", line)
            if m:
                counts[os.path.relpath(m.group(1), ROOT)] += 1
    return dict(counts)


def read_baseline() -> dict[str, int]:
    out = {}
    if os.path.exists(BASELINE):
        with open(BASELINE) as f:
            for line in f:
                if line.strip() and not line.startswith("#"):
                    n, path = line.split(None, 1)
                    out[path.strip()] = int(n)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--doxygen", default="doxygen")
    ap.add_argument("--update", action="store_true", help="write the current counts as the baseline")
    ap.add_argument("--strict", action="store_true", help="exit 1 when a header has more than its baseline")
    a = ap.parse_args()
    now, base = count(a.doxygen), read_baseline()
    if a.update:
        with open(BASELINE, "w") as f:
            f.write("# Undocumented declarations per header (scripts/check_api_docs.py --update); may only go down.\n")
            for path in sorted(now):
                f.write(f"{now[path]} {path}\n")
        print(f"baseline: {sum(now.values())} undocumented declarations in {len(now)} headers")
        return 0
    worse = {p: n for p, n in now.items() if n > base.get(p, 0)}
    for path, n in sorted(worse.items()):
        msg = f"{n - base.get(path, 0)} new undocumented declaration(s) (now {n}); put a comment above each"
        print(f"::warning file={path}::{msg}" if os.environ.get("GITHUB_ACTIONS") else f"{path}: {msg}")
    better = sum(base.get(p, 0) - now.get(p, 0) for p in base if now.get(p, 0) < base[p])
    print(f"undocumented declarations: {sum(now.values())} (baseline {sum(base.values())}); "
          f"{len(worse)} header(s) above baseline" + (f"; {better} fewer elsewhere: run --update" if better else ""))
    return 1 if worse and a.strict else 0


if __name__ == "__main__":
    sys.exit(main())
