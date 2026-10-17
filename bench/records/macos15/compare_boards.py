#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Board-by-board comparison of two bench/run.py runs made on different machines.

  compare_boards.py REFERENCE_BOARDS_JSONL OTHER_BOARDS_JSONL

bench/compare_runs.py compares the routed files themselves, which only exist on the machine that made them; a run
copied into bench/records keeps boards.jsonl only. This compares what boards.jsonl holds for each board: routed
connections, tracks, vias, completion, clean, added errors and the style metric (lengths in nm-exact millimetres,
bends, segments). Equal fingerprints on every board do not prove byte-identical files, but a different route
almost never keeps all of them. Exit status 1 when any board differs or the board sets differ.
"""
import json
import sys

FIELDS = ("routed", "connections", "tracks", "vias", "completion", "clean", "added_errors", "unconnected_after")


def load(path):
    return {r["board"]: r for r in map(json.loads, open(path))}


def fingerprint(r):
    return {**{k: r.get(k) for k in FIELDS}, "style": r.get("style")}


def main():
    ref, other = load(sys.argv[1]), load(sys.argv[2])
    status = 0
    if set(ref) != set(other):
        print(f"board sets differ: only in reference {sorted(set(ref) - set(other))}, only in other {sorted(set(other) - set(ref))}")
        status = 1
    same = 0
    for b in sorted(set(ref) & set(other)):
        a, o = fingerprint(ref[b]), fingerprint(other[b])
        if a == o:
            same += 1
            continue
        status = 1
        diff = {k: (a[k], o[k]) for k in a if a[k] != o[k] and k != "style"}
        if a["style"] != o["style"]:
            diff["style"] = {k: (a["style"].get(k), o["style"].get(k)) for k in a["style"] if a["style"].get(k) != o["style"].get(k)}
        print(f"DIFF {b}: {diff}")
    n = len(set(ref) & set(other))
    clean = lambda d: sum(1 for b in d if d[b].get("clean"))
    print(f"{same}/{n} boards with identical fingerprints; clean {clean(ref)} -> {clean(other)}")
    return status


if __name__ == "__main__":
    sys.exit(main())
