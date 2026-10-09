#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Paired comparison of two bench/run.py runs on the same boards (doc 10 §4, gates of §5).

  bench/compare_runs.py BEFORE_RUN AFTER_RUN [--expect-identical] [--markdown FILE]

A run is a directory bench/results/<run-id> or just the run id. Per board: routed boards byte-identical or not,
completion and clean pass before and after, added DRC errors. Exit status 1 when a gate fails:
  1. no board gets worse in clean pass or completion;
  2. no added KiCad DRC errors on a board that was clean before;
  3. both runs hold the same boards, and at least one;
  with --expect-identical (speed-ups, refactors): every routed board byte-identical.
Wall time is reported, not gated: it is only comparable from the same machine and load.
"""
import argparse
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


def load(run: str) -> tuple[pathlib.Path, dict]:
    d = pathlib.Path(run)
    if not d.is_dir():
        d = ROOT / "bench/results" / run
    rows = {}
    for line in (d / "boards.jsonl").read_text().splitlines():
        if line.strip():
            r = json.loads(line)
            rows[r["board"]] = r
    return d, rows


def digest(path: pathlib.Path) -> str | None:
    return hashlib.sha256(path.read_bytes()).hexdigest() if path.exists() else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("before")
    ap.add_argument("after")
    ap.add_argument("--expect-identical", action="store_true", help="fail unless every routed board is byte-identical")
    ap.add_argument("--markdown", help="also write the report here")
    a = ap.parse_args()
    bdir, before = load(a.before)
    adir, after = load(a.after)
    boards = sorted(set(before) & set(after))
    missing = sorted(set(before) ^ set(after))
    identical, rows, failures = 0, [], []
    for name in boards:
        b, f = before[name], after[name]
        db, df = digest(bdir / "boards" / f"{name}.kicad_pcb"), digest(adir / "boards" / f"{name}.kicad_pcb")
        same = db is not None and db == df
        identical += same
        if "clean" not in b or "clean" not in f:
            failures.append(f"{name}: judge failed ({'before' if 'clean' not in b else 'after'})")
            continue
        worse = (b["clean"] and not f["clean"]) or f["completion"] < b["completion"]
        added = f["added_errors"] if b["clean"] and f["added_errors"] else {}
        if worse:
            failures.append(f"{name}: worse (clean {b['clean']} -> {f['clean']}, completion {b['completion']:.1%} -> {f['completion']:.1%})")
        if added:
            failures.append(f"{name}: added DRC errors on a board clean before: {added}")
        if not same and a.expect_identical:
            failures.append(f"{name}: routed board differs")
        if not same or worse or added:
            rows.append(f"| {name} | {b.get('routed', '?')} → {f.get('routed', '?')} / {f.get('connections', '?')} | "
                        f"{b['completion']:.1%} → {f['completion']:.1%} | {'yes' if b['clean'] else 'no'} → {'yes' if f['clean'] else 'no'} | "
                        f"{f['added_errors'] or '–'} | {b.get('seconds', 0):.1f} → {f.get('seconds', 0):.1f} | {'identical' if same else 'differs'} |")

    def mean(rs, key):
        vals = [r[key] for r in rs if key in r]
        return sum(vals) / len(vals) if vals else 0.0

    bs, fs = [before[n] for n in boards], [after[n] for n in boards]
    out = [f"# {bdir.name} → {adir.name}", "",
           f"{len(boards)} boards; routed boards byte-identical: {identical}/{len(boards)}.",
           f"Clean pass {sum(r.get('clean', False) for r in bs)} → {sum(r.get('clean', False) for r in fs)}; "
           f"mean completion {mean(bs, 'completion'):.1%} → {mean(fs, 'completion'):.1%}; "
           f"router seconds {sum(r.get('seconds', 0) for r in bs):.1f} → {sum(r.get('seconds', 0) for r in fs):.1f}."]
    # A run that lost a board (crash, judge failure) or shares none with the other must not pass by having nothing
    # to compare.
    if missing:
        out.append(f"In one run only (not compared): {', '.join(missing)}.")
        failures.append(f"{len(missing)} boards are in one run only")
    if not boards:
        failures.append("no board is in both runs")
    if rows:
        out += ["", "| Board | Routed | Completion | Clean | Added errors (after) | Router s | Output |", "|---|---|---|---|---|---|---|"] + rows
    out += ["", "Gates: " + ("pass" if not failures else "FAIL")] + [f"- {x}" for x in failures]
    text = "\n".join(out) + "\n"
    print(text, end="")
    if a.markdown:
        pathlib.Path(a.markdown).write_text(text)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
