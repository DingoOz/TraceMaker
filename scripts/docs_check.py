#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the documentation's page headers and warns about docs the code may have outgrown (D134).

  scripts/docs_check.py [--stale-after N] [--strict]

Every page under docs/ (decision records and generated pages aside) starts with a YAML header:

  ---
  status: current          # current | partly-built | plan | historical
  code: [src/route, src/passes]   # the code the page describes; [] for none
  reviewed: 2026-10-21     # optional: the date someone last checked the page against the code
  ---

A missing or malformed header, an unknown status or a `code` path that does not exist is an error. A page is
*possibly stale* when its code has had more than N commits (default 25) since the later of the page's own last
commit and its `reviewed` date; that is a warning (a GitHub annotation in CI), never an error unless --strict.
Read the page against the code, fix what is wrong, and set `reviewed` to today's date.
"""
import argparse
import os
import re
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOCS = os.path.join(ROOT, "docs")
STATUSES = ("current", "partly-built", "plan", "historical")
SKIP_DIRS = {"decisions", "footprint_refs", "img", "assets", "api"}
GENERATED = {"reference/cli.md", "reference/benchmarks.md"}


def header(path: str) -> dict | None:
    with open(path, encoding="utf-8") as f:
        m = re.match(r"^---\n(.*?)\n---\n", f.read(), re.S)
    if not m:
        return None
    meta = {}
    for line in m.group(1).split("\n"):
        key, _, value = line.partition(":")
        value = value.split(" #")[0].strip()
        meta[key.strip()] = [v.strip() for v in value.strip("[]").split(",") if v.strip()] if value.startswith("[") else value
    return meta


def git(*args: str) -> str:
    return subprocess.run(["git", *args], cwd=ROOT, capture_output=True, text=True, check=True).stdout.strip()


def pages() -> list[str]:
    out = []
    for dirpath, dirnames, filenames in os.walk(DOCS):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            rel = os.path.relpath(os.path.join(dirpath, name), DOCS)
            if name.endswith(".md") and rel not in GENERATED:
                out.append(rel)
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--stale-after", type=int, default=25, help="code commits since the page before it is flagged")
    ap.add_argument("--strict", action="store_true", help="possibly stale pages are errors too")
    a = ap.parse_args()
    errors, stale = [], []
    for rel in pages():
        meta = header(os.path.join(DOCS, rel))
        if meta is None:
            errors.append(f"docs/{rel}: no YAML header (status, code)")
            continue
        if meta.get("status") not in STATUSES:
            errors.append(f"docs/{rel}: status {meta.get('status')!r} is not one of {', '.join(STATUSES)}")
        code = meta.get("code")
        if not isinstance(code, list):
            errors.append(f"docs/{rel}: `code` must be a list ([] for none)")
            continue
        missing = [c for c in code if not os.path.exists(os.path.join(ROOT, c))]
        if missing:
            errors.append(f"docs/{rel}: code path(s) {', '.join(missing)} do not exist")
            continue
        if not code or meta.get("status") == "historical":
            continue
        since = git("log", "-1", "--format=%cI", "--", f"docs/{rel}")
        if not since:
            continue  # not committed yet
        if meta.get("reviewed", "") > since[:10]:
            since = meta["reviewed"]
        n = len([c for c in git("log", "--format=%H", f"--since={since}", "--", *code).split("\n") if c])
        if n > a.stale_after:
            stale.append((rel, n, since[:10], code))
    for e in errors:
        print(e, file=sys.stderr)
    ci = os.environ.get("GITHUB_ACTIONS")
    for rel, n, since, code in stale:
        msg = f"possibly stale: {n} commits to {', '.join(code)} since {since}; check it and set `reviewed`"
        print(f"::warning file=docs/{rel}::{msg}" if ci else f"docs/{rel}: {msg}")
    print(f"{len(pages())} pages, {len(errors)} error(s), {len(stale)} possibly stale")
    return 1 if errors or (stale and a.strict) else 0


if __name__ == "__main__":
    sys.exit(main())
