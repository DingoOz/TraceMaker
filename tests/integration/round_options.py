#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The round-based options run with one variant (M17, D122): `--variants 1 --relax` used to be ignored without a word. And the relax
rescue (D160) that a plain run gets when it leaves connections open.

  round_options.py <tracemaker> <work dir>

Routes tests/boards/plane_smd with a small budget:
- `--variants 1 --relax --rounds 1` must print the clearance stages of the relax portfolio ("ramp stage"), and `--variants 1 --no-rescue`
  must not (the plain single router, unchanged);
- `--variants 1` (the rescue on, as by default) leaves the board no worse than `--no-rescue`: with connections open the rescue runs and
  says so in the summary; when it does not win, the output is byte for byte the plain run's. Its passes are warm, then fresh while
  connections are open (D163).
"""
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "tests/boards/plane_smd/plane_smd.kicad_pcb"


def route(tm: str, work: pathlib.Path, name: str, extra: list[str]) -> tuple[str, dict]:
    js = work / f"{name}.json"
    r = subprocess.run([tm, "route", str(BOARD), "-o", str(work / f"{name}.kicad_pcb"), "--json", str(js), "--work", "300000", "--time", "600",
                        "--variants", "1", "--threads", "1", "--no-kb", "--no-gpu"] + extra, capture_output=True, text=True)
    if r.returncode not in (0, 3):
        sys.exit(f"{name}: tracemaker failed with {r.returncode}\n{r.stdout[-1500:]}\n{r.stderr[-1500:]}")
    return r.stdout + r.stderr, json.loads(js.read_text())


def main() -> int:
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    plain, plain_js = route(tm, work, "plain", ["--no-rescue"])
    relax, _ = route(tm, work, "relax", ["--relax", "--rounds", "1"])
    rescued, rescued_js = route(tm, work, "rescued", [])
    bad = []
    if "ramp stage" in plain:
        bad.append("the plain single router printed clearance stages")
    if "ramp stage" not in relax:
        bad.append("--variants 1 --relax printed no clearance stage: the option was ignored")
    if "rescue" in plain_js:
        bad.append("--no-rescue still ran the rescue")
    if plain_js["routed"] < plain_js["connections"]:
        rs = rescued_js.get("rescue")
        if not rs:
            bad.append(f"the plain run left {plain_js['connections'] - plain_js['routed']} connections open and no rescue ran")
        elif rs["routed_before"] != plain_js["routed"]:
            bad.append(f"the rescue started from {rs['routed_before']} routed, the plain run routed {plain_js['routed']}")
        elif not rs["adopted"] and (work / "rescued.kicad_pcb").read_bytes() != (work / "plain.kicad_pcb").read_bytes():
            bad.append("the rescue did not win, yet the output differs from the plain run's")
        else:
            modes = [p["mode"] for p in rs.get("passes", [])]
            # Default --rescue-mode both (D163): warm first, then fresh only while connections are open.
            if not modes or modes[0] != "warm" or modes not in (["warm"], ["warm", "fresh"]):
                bad.append(f"rescue passes {modes}: expected warm, then fresh while connections are open")
            elif modes == ["warm"] and rescued_js["routed"] < rescued_js["connections"]:
                bad.append("connections were open after the warm pass and no fresh pass ran")
    elif "rescue" in rescued_js:
        bad.append("a complete board got a rescue")
    if rescued_js["routed"] < plain_js["routed"]:
        bad.append(f"the rescue made the board worse: {plain_js['routed']} -> {rescued_js['routed']}")
    for b in bad:
        print("FAIL:", b)
    if not bad:
        rs = rescued_js.get("rescue")
        print("PASS: --relax runs its stages with one variant; the plain run is unchanged; rescue",
              f"{rs['routed_before']} -> {rescued_js['routed']} (kept: {rs['adopted']}; passes {[(p['mode'], p['routed']) for p in rs['passes']]})" if rs else "not needed")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
