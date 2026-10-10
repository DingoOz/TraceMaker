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
Wall time and route CPU time are reported, not gated: they are only comparable from the same machine. When both runs
used the same --work budget, boards whose routed output is byte-identical did the same work, so their CPU change is
run-to-run noise or a change in speed that leaves the output alone; the report gives that band and lists the other
boards (and the total) that moved beyond it, at least 10 %. One pair of runs cannot tell the two apart: when every
identical board moved the same way by more than 10 % the report says so, and a second run of one binary settles it.
Equal work is not equal time (the budget counts search expansions and flood cells only, doc 10 §5), so a claim
about speed needs a run at equal wall time.
"""
import argparse
import hashlib
import json
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent


CPU_FLAG = 0.10  # smallest relative change in route CPU time at equal work that the report points out


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
    identical, rows, failures, notes = 0, [], [], []
    noise: list[float] = []  # CPU change of byte-identical boards: same work, so run-to-run noise or a uniform speed change

    def summary(d: pathlib.Path) -> dict:
        f = d / "summary.json"
        return json.loads(f.read_text()) if f.exists() else {}

    same_work = bool(summary(bdir).get("work")) and summary(bdir).get("work") == summary(adir).get("work")

    def cpu_change(b: dict, f: dict) -> float | None:
        cb, cf = b.get("cpu_s"), f.get("cpu_s")
        return (cf - cb) / cb if cb and cf is not None else None
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
        dc = cpu_change(b, f)
        if same and dc is not None:
            noise.append(dc)
        if not same or worse or added:
            cpu = f"{b['cpu_s']:.1f} → {f['cpu_s']:.1f}" if dc is not None else "–"
            rows.append(f"| {name} | {b.get('routed', '?')} → {f.get('routed', '?')} / {f.get('connections', '?')} | "
                        f"{b['completion']:.1%} → {f['completion']:.1%} | {'yes' if b['clean'] else 'no'} → {'yes' if f['clean'] else 'no'} | "
                        f"{f['added_errors'] or '–'} | {b.get('seconds', 0):.1f} → {f.get('seconds', 0):.1f} | {cpu} | {'identical' if same else 'differs'} |")

    def mean(rs, key):
        vals = [r[key] for r in rs if key in r]
        return sum(vals) / len(vals) if vals else 0.0

    bs, fs = [before[n] for n in boards], [after[n] for n in boards]
    out = [f"# {bdir.name} → {adir.name}", "",
           f"{len(boards)} boards; routed boards byte-identical: {identical}/{len(boards)}.",
           f"Clean pass {sum(r.get('clean', False) for r in bs)} → {sum(r.get('clean', False) for r in fs)}; "
           f"mean completion {mean(bs, 'completion'):.1%} → {mean(fs, 'completion'):.1%}; "
           f"router seconds {sum(r.get('seconds', 0) for r in bs):.1f} → {sum(r.get('seconds', 0) for r in fs):.1f}."]
    cpu_b, cpu_f = sum(r.get("cpu_s", 0) for r in bs), sum(r.get("cpu_s", 0) for r in fs)
    if cpu_b and all("cpu_s" in r for r in bs + fs):
        out.append(f"Route CPU time {cpu_b:,.0f} → {cpu_f:,.0f} s ({(cpu_f - cpu_b) / cpu_b:+.1%})" + (" at the same work." if same_work else "."))
        if same_work:
            band = max([CPU_FLAG] + [abs(x) for x in noise])
            if noise:
                out.append(f"Byte-identical boards moved {min(noise):+.0%} to {max(noise):+.0%} in CPU time (the same work: noise, or speed).")
            # Noise scatters around zero. A speed-up or slow-down that leaves the output alone moves every identical
            # board the same way, and the band above would hide it.
            if noise and (min(noise) > CPU_FLAG or max(noise) < -CPU_FLAG):
                notes.append(f"every byte-identical board moved the same way, {min(noise):+.0%} to {max(noise):+.0%}: a change in speed "
                             "(binary or machine), not scatter; run one binary twice to tell which")
            if abs(cpu_f - cpu_b) / cpu_b > band:
                notes.append(f"route CPU time {(cpu_f - cpu_b) / cpu_b:+.1%} in total")
            for name in boards:
                dc = cpu_change(before[name], after[name])
                if dc is not None and abs(dc) > band:
                    notes.append(f"{name}: route CPU time {before[name]['cpu_s']:.1f} → {after[name]['cpu_s']:.1f} s ({dc:+.0%})")
    # A run that lost a board (crash, judge failure) or shares none with the other must not pass by having nothing
    # to compare.
    if missing:
        out.append(f"In one run only (not compared): {', '.join(missing)}.")
        failures.append(f"{len(missing)} boards are in one run only")
    if not boards:
        failures.append("no board is in both runs")
    if rows:
        out += ["", "| Board | Routed | Completion | Clean | Added errors (after) | Router s | CPU s | Output |", "|---|---|---|---|---|---|---|---|"] + rows
    if notes:
        out += ["", "CPU time at equal work, beyond what the byte-identical boards show as noise (not a gate; check speed at equal wall time, doc 10 §5):"] + [f"- {x}" for x in notes]
    out += ["", "Gates: " + ("pass" if not failures else "FAIL")] + [f"- {x}" for x in failures]
    text = "\n".join(out) + "\n"
    print(text, end="")
    if a.markdown:
        pathlib.Path(a.markdown).write_text(text)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
