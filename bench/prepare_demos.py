#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Prepare KiCad demo projects (and test boards) as bench/run.py fixtures: <out>/<name>/unrouted.kicad_pcb with
tracks, arcs and vias removed (zones and their fills kept), next to copies of the project and custom rules, so the
router and KiCad's judge read the same net classes and rules.

  bench/prepare_demos.py [--out build/demos] [--set planes | NAME ...]

A name is a board file stem looked up in bench/data/kicad/demos/*/ (scripts/fetch_fixtures.sh kicad-demos) and
tests/boards/*/. Prints the fixture directory for bench/run.py --fixtures.
"""
import argparse
import pathlib
import shutil
import sys

from prepare_dac2020 import strip_routing

ROOT = pathlib.Path(__file__).resolve().parent.parent
SEARCH = [ROOT / "bench/data/kicad/demos", ROOT / "tests/boards"]


def find(name: str) -> pathlib.Path:
    for base in SEARCH:
        hits = sorted(base.glob(f"*/{name}.kicad_pcb"))
        if hits:
            return hits[0]
    raise SystemExit(f"board not found: {name} (looked in {', '.join(str(s) for s in SEARCH)})")


def prepare(name: str, out: pathlib.Path) -> pathlib.Path:
    src = find(name)
    dst = out / name / "unrouted.kicad_pcb"
    dst.parent.mkdir(parents=True, exist_ok=True)
    for ext in (".kicad_pro", ".kicad_dru"):
        if src.with_suffix(ext).exists():
            shutil.copyfile(src.with_suffix(ext), dst.with_suffix(ext))
    dst.write_text(strip_routing(src.read_text()))
    return dst


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=str(ROOT / "build/demos"))
    ap.add_argument("--set", help="board set file name in bench/sets (e.g. planes)")
    ap.add_argument("names", nargs="*")
    a = ap.parse_args()
    names = list(a.names)
    if a.set:
        names += [x.strip() for x in (ROOT / "bench/sets" / f"{a.set}.txt").read_text().splitlines() if x.strip() and not x.startswith("#")]
    if not names:
        ap.error("give --set or board names")
    out = pathlib.Path(a.out).resolve()
    for n in names:
        prepare(n, out)
    print(out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
