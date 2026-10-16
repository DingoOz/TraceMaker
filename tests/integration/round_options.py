#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The round-based options run with one variant (M17, D122): `--variants 1 --relax` used to be ignored without a word.

  round_options.py <tracemaker> <work dir>

Routes tests/boards/plane_smd twice with a small budget: `--variants 1 --relax --rounds 1` must print the clearance stages of the relax
portfolio ("ramp stage"), and `--variants 1` alone must not (the plain single router, unchanged).
"""
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "tests/boards/plane_smd/plane_smd.kicad_pcb"


def route(tm: str, work: pathlib.Path, name: str, extra: list[str]) -> str:
    r = subprocess.run([tm, "route", str(BOARD), "-o", str(work / f"{name}.kicad_pcb"), "--work", "300000", "--time", "600", "--variants", "1", "--threads", "1",
                        "--no-kb", "--no-gpu"] + extra, capture_output=True, text=True)
    if r.returncode not in (0, 3):
        sys.exit(f"{name}: tracemaker failed with {r.returncode}\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}")
    return r.stdout + r.stderr


def main() -> int:
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    plain = route(tm, work, "plain", [])
    relax = route(tm, work, "relax", ["--relax", "--rounds", "1"])
    bad = []
    if "ramp stage" in plain:
        bad.append("the plain single router printed clearance stages")
    if "ramp stage" not in relax:
        bad.append("--variants 1 --relax printed no clearance stage: the option was ignored")
    for b in bad:
        print("FAIL:", b)
    if not bad:
        print("PASS: --relax runs its stages with one variant; the plain run is unchanged")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
