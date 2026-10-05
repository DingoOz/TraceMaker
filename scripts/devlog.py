#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Append an entry to the development activity log shown on the progress website.

Usage: scripts/devlog.py [--kind build|test|note|decision|milestone] "message"
"""
import argparse, datetime, json, pathlib

LOG = pathlib.Path(__file__).resolve().parent.parent / "dev" / "activity.jsonl"

def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("--kind", default="note", choices=["build", "test", "note", "decision", "milestone"])
    ap.add_argument("text")
    a = ap.parse_args()
    entry = {"ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"), "kind": a.kind, "text": a.text}
    LOG.parent.mkdir(exist_ok=True)
    with LOG.open("a") as f:
        f.write(json.dumps(entry) + "\n")

if __name__ == "__main__":
    main()
