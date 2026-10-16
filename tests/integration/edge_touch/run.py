#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Copper against the board outline at zero (and small) required clearance: TraceMaker's DRC against frozen KiCad results (issue #42, D120).

  run.py <tracemaker> <work-dir>
  run.py <tracemaker> <work-dir> --rejudge [--kicad-cli kicad-cli]    # measure KiCad again and rewrite expected.json

Each case is a 10 mm square outline with one item whose copper edge lies a few nanometres inside, on, or outside the outline's centre line.
Only --rejudge needs KiCad (exit 77 when its executable is absent); work-dir has to be somewhere kicad-cli can read (not /tmp).
KiCad's result differs by shape at zero clearance: straight tracks, vias and circle, oval and round-rect pads are flagged only once they cross the
centre line, rect and trapezoid pads and arc tracks already when they touch it. A frozen oracle is the only way to keep that from drifting.
"""
import argparse
import json
import pathlib
import shutil
import subprocess
import sys

PCB_HEAD = '''(kicad_pcb
	(version 20260206)
	(generator "pcbnew")
	(generator_version "10.0")
	(general (thickness 1.6) (legacy_teardrops no))
	(paper "A4")
	(layers (0 "F.Cu" signal) (2 "B.Cu" signal) (25 "Edge.Cuts" user))
	(setup (pad_to_mask_clearance 0))
'''
RECT = '\t(gr_rect (start -5 -5) (end 5 5) (stroke (width 0.05) (type default)) (fill no) (layer "Edge.Cuts") (uuid "11111111-1111-4111-8111-111111111111"))\n'
LINES = ''.join(f'\t(gr_line (start {a}) (end {b}) (stroke (width 0.05) (type default)) (layer "Edge.Cuts") (uuid "1111111{i}-1111-4111-8111-111111111111"))\n'
                for i, (a, b) in enumerate([("-5 -5", "5 -5"), ("5 -5", "5 5"), ("5 5", "-5 5"), ("-5 5", "-5 -5")]))
PRO = '{"board":{"design_settings":{"rules":{"min_clearance":0.1,"min_track_width":0.12,"min_copper_edge_clearance":%s}}},"meta":{"filename":"x.kicad_pro","version":3}}'
UUID = "22222222-2222-4222-8222-222222222222"

# Offset of the copper edge past the outline's centre line, in nanometres: inside by 1, touching, crossing by 1, 499, 500, 501 and 1000.
OFFSETS = {"in1": -1, "touch": 0, "x1": 1, "x499": 499, "x500": 500, "x501": 501, "x1000": 1000}


def mm(nm: int) -> str:
    return f"{nm / 1e6:.6f}".rstrip("0").rstrip(".")


def pad(shape: str, size: str, extra: str = ""):
    # A pad of half height 0.5 mm, its top side (or top of the circle) `offset` past y = 5.
    return lambda off: (f'\t(footprint "R" (layer "F.Cu") (at 0 {mm(4_500_000 + off)}) (uuid "{UUID}")\n'
                        f'\t\t(pad "1" smd {shape} (at 0 0) (size {size}) {extra}(layers "F.Cu") (uuid "44444444-4444-4444-8444-444444444444")))\n')


ITEMS = {
    # A track 0.12 mm wide: its copper edge is 0.06 mm from its centre line.
    "track": lambda off: f'\t(segment (start -3 {mm(4_940_000 + off)}) (end 3 {mm(4_940_000 + off)}) (width 0.12) (layer "F.Cu") (uuid "{UUID}"))\n',
    "via": lambda off: f'\t(via (at 0 {mm(4_500_000 + off)}) (size 1) (drill 0.5) (layers "F.Cu" "B.Cu") (net 0) (uuid "{UUID}"))\n',
    # Circle through (+-2.964, 3.952) and (0, 4.94): centre at the origin, radius 4.94 (a 3-4-5 triangle scaled by 0.988), exact in nanometres.
    "arc": lambda off: (f'\t(arc (start -2.964 {mm(3_952_000 + off)}) (mid 0 {mm(4_940_000 + off)}) (end 2.964 {mm(3_952_000 + off)}) (width 0.12) '
                        f'(layer "F.Cu") (uuid "{UUID}"))\n'),
    "pad_rect": pad("rect", "1 1"),
    "pad_trapezoid": pad("trapezoid", "1 1"),
    "pad_circle": pad("circle", "1 1"),
    "pad_oval": pad("oval", "2 1"),
    "pad_roundrect": pad("roundrect", "1 1", "(roundrect_rratio 0.25) "),
}


def generate(root: pathlib.Path) -> dict:
    cases = {}

    def case(name: str, outline: str, item: str, clearance: str):
        d = root / name
        d.mkdir(parents=True, exist_ok=True)
        (d / "x.kicad_pcb").write_text(PCB_HEAD + outline + item + ")\n")
        (d / "x.kicad_pro").write_text(PRO % clearance)
        cases[name] = d / "x.kicad_pcb"

    for kind, make in ITEMS.items():
        for tag, off in OFFSETS.items():
            case(f"{kind}_{tag}", RECT, make(off), "0")
    for kind in ("track", "via", "pad_rect"):  # an outline of four lines has the same result as a rectangle
        for tag in ("in1", "touch", "x1"):
            case(f"{kind}_{tag}_lines", LINES, ITEMS[kind](OFFSETS[tag]), "0")
    # A required clearance of 0.1 mm: the gap between the track's copper edge and the centre line, 100 um minus 0, 1, 499, 500, 501 and 1000 nm.
    for tag, less in {"g0": 0, "g1": 1, "g499": 499, "g500": 500, "g501": 501, "g1000": 1000}.items():
        case(f"track_clearance_{tag}", RECT, ITEMS["track"](-100_000 + less), "0.1")
    return cases


def engine_count(tracemaker: str, board: pathlib.Path) -> int:
    out = board.with_name("t.json")
    out.unlink(missing_ok=True)
    subprocess.run([tracemaker, "drc", str(board), "--json", str(out)], capture_output=True, timeout=120)
    return sum(1 for v in json.loads(out.read_text())["violations"] if v["type"] == "copper_edge_clearance")


def kicad_count(kicad_cli: str, board: pathlib.Path) -> int:
    out = board.with_name("k.json")
    out.unlink(missing_ok=True)
    subprocess.run([kicad_cli, "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "-o", str(out), str(board)],
                   capture_output=True, timeout=300, cwd=board.parent)
    return sum(1 for v in json.loads(out.read_text())["violations"] if v["type"] == "copper_edge_clearance")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tracemaker")
    ap.add_argument("work_dir", type=pathlib.Path)
    ap.add_argument("--rejudge", action="store_true")
    ap.add_argument("--kicad-cli", default="kicad-cli")
    ap.add_argument("--expected", type=pathlib.Path, default=pathlib.Path(__file__).with_name("expected.json"))
    args = ap.parse_args()
    cases = generate(args.work_dir.resolve())
    if args.rejudge:
        if shutil.which(args.kicad_cli) is None:
            print("SKIP: kicad-cli missing")
            return 77
        counts = {name: kicad_count(args.kicad_cli, board) for name, board in cases.items()}
        args.expected.write_text(json.dumps({"schema_version": 1, "judge": "kicad-cli 10.0.6", "copper_edge_clearance": counts}, indent=1, sort_keys=True) + "\n")
        print(f"wrote {args.expected} ({len(counts)} cases)")
        return 0
    frozen = json.loads(args.expected.read_text())
    if frozen.get("schema_version") != 1 or set(frozen["copper_edge_clearance"]) != set(cases):
        print("FAIL: expected.json does not list exactly the generated cases; run with --rejudge")
        return 1
    bad = 0
    for name, board in sorted(cases.items()):
        got, want = engine_count(args.tracemaker, board), frozen["copper_edge_clearance"][name]
        if got != want:
            bad += 1
            print(f"FAIL {name}: TraceMaker reports {got} copper_edge_clearance, KiCad ({frozen['judge']}) {want}")
    print(f"edge_touch: {len(cases) - bad}/{len(cases)} cases agree with {frozen['judge']}")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
