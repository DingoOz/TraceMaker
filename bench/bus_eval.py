#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bus evaluation (doc 05 §38): routes boards without and with bus routing at the same deterministic budget and
reports, per group, what `tracemaker buses` measures on the written boards (members routed, vias per member, members
off the group's layers, skew against the limit), and per board the completion and the KiCad DRC errors routing added.

  bench/bus_eval.py --fixtures DIR [--work 2000000] [--jobs 4] [--drc-jobs 2] [--out build/bus_eval] [--json FILE] [--no-drc]
  bench/bus_eval.py --board NAME_OR_PATH --bus-args "--bus 'D=/D[0-7]'" [--board ... --bus-args ...]

--fixtures takes a directory written by bench/make_bus_boards.py (its manifest.json gives each board's bus options).
--board takes a PCBench fixture name or a .kicad_pcb path, each followed by its own --bus-args. Both runs use
--variants 1 --threads 1 --no-kb --no-gpu and differ only by the bus options. The measurement of the "off" board uses
the same groups and limits, so the two rows compare like with like. Boards are copied under --out with their project
and rule files, because kicad-cli reads the rules from the files beside the board.
"""
import argparse
import concurrent.futures as cf
import importlib.util
import json
import pathlib
import shlex
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
TM = ROOT / "build/release/src/app/tracemaker"
_spec = importlib.util.spec_from_file_location("bench_run", ROOT / "bench/run.py")
bench_run = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(bench_run)
PCBENCH = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"


def stage(src: pathlib.Path, dst: pathlib.Path) -> None:
    """Copies a board with its .kicad_pro / .kicad_dru to the stem of `dst`."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if src != dst:
        shutil.copy(src, dst)
    for ext in (".kicad_pro", ".kicad_dru"):
        side = src.with_suffix(ext)
        if side.exists() and side != dst.with_suffix(ext):
            shutil.copy(side, dst.with_suffix(ext))


def route(src: pathlib.Path, out: pathlib.Path, work: int, extra: list[str]) -> dict:
    js = out.with_suffix(".json")
    cmd = [str(TM), "route", str(src), "-o", str(out), "--work", str(work), "--time", "3600", "--variants", "1", "--threads", "1", "--no-kb",
           "--no-gpu", "--json", str(js), *extra]
    r = subprocess.run(cmd, capture_output=True, text=True)
    out.with_suffix(".log").write_text(r.stdout + r.stderr)
    if not js.exists():  # the router exits 3 when connections stay unrouted; only a missing summary is an error
        return {"error": r.returncode}
    return json.loads(js.read_text())


def measure(board: pathlib.Path, bus_args: list[str]) -> list[dict]:
    js = board.with_suffix(".buses.json")
    r = subprocess.run([str(TM), "buses", str(board), "--json", str(js), *bus_args], capture_output=True, text=True)
    if r.returncode not in (0, 4):
        raise SystemExit(f"tracemaker buses failed on {board}: {r.stderr}")
    return json.loads(js.read_text())["buses"]


def added_errors(before: dict, after: dict) -> dict:
    return {t: n - before["routed_errors"].get(t, 0) for t, n in after["routed_errors"].items()
            if t not in bench_run.NOT_ROUTING and n - before["routed_errors"].get(t, 0) > 0}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--fixtures")
    ap.add_argument("--board", action="append", default=[])
    ap.add_argument("--bus-args", action="append", default=[])
    ap.add_argument("--work", type=int, default=2_000_000)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--drc-jobs", type=int, default=2)
    ap.add_argument("--out", default=str(ROOT / "build/bus_eval"))
    ap.add_argument("--route-args", default="", help="extra options for both runs")
    ap.add_argument("--no-drc", action="store_true")
    ap.add_argument("--json")
    a = ap.parse_args()
    out = pathlib.Path(a.out).resolve()
    cases = []  # (tag, source board, bus options, gated)
    if a.fixtures:
        fx = pathlib.Path(a.fixtures)
        for m in json.loads((fx / "manifest.json").read_text()):
            cases.append((m["name"], fx / m["board"], m["bus_args"], m["gate"]))
    if len(a.board) != len(a.bus_args):
        raise SystemExit("every --board needs its --bus-args")
    for name, args in zip(a.board, a.bus_args):
        p = pathlib.Path(name)
        src = p if p.suffix == ".kicad_pcb" else PCBENCH / name / "unrouted.kicad_pcb"
        if not src.exists():
            raise SystemExit(f"no board {name}")
        cases.append((p.stem if p.suffix == ".kicad_pcb" else name, src, shlex.split(args), False))
    common = shlex.split(a.route_args)
    jobs = []
    for tag, src, bus_args, gated in cases:
        base = out / tag / f"{tag}.kicad_pcb"
        stage(src, base)
        for mode in ("off", "on"):
            dst = out / tag / f"{tag}.{mode}.kicad_pcb"
            stage(base, dst)
            dst.unlink()
            jobs.append((tag, mode, base, dst, common + (bus_args if mode == "on" else [])))
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        res = dict(zip([(j[0], j[1]) for j in jobs], ex.map(lambda j: route(j[2], j[3], a.work, j[4]), jobs)))
    drcs = {}
    if not a.no_drc and shutil.which("kicad-cli"):
        files = sorted({j[2] for j in jobs}) + [j[3] for j in jobs if j[3].exists()]
        with cf.ThreadPoolExecutor(a.drc_jobs) as ex:
            drcs = dict(zip(files, ex.map(bench_run.drc, files)))
    rows = []
    for tag, src, bus_args, gated in cases:
        for mode in ("off", "on"):
            dst = out / tag / f"{tag}.{mode}.kicad_pcb"
            r = res[(tag, mode)]
            row = {"board": tag, "mode": mode, "gate": gated, "routed": r.get("routed"), "connections": r.get("connections"),
                   "vias": r.get("vias"), "expansions": r.get("expansions"), "buses": measure(dst, bus_args) if dst.exists() else []}
            before, after = drcs.get(out / tag / f"{tag}.kicad_pcb"), drcs.get(dst)
            if before and after:
                row["unconnected"] = after["unconnected"]
                row["added_errors"] = added_errors(before, after)
            rows.append(row)
    print(f"{'board':10} {'mode':4} {'routed':>9} {'vias':>4} {'added':>5}  group: routed, vias per member min-max, left the bus, skew / limit")
    for r in rows:
        gs = "; ".join(
            f"{g['name']}: {g['routed']}/{g['nets']}, vias {g['vias_min']}-{g['vias_max']}, left {g['left']}, skew {g['skew_mm']:.3f}"
            + (f" / {g['skew_limit_mm']:.3f} {'ok' if g['within_skew'] else 'OVER'}" if g["skew_limit_mm"] is not None else " / none")
            for g in r["buses"])
        add = sum(r["added_errors"].values()) if "added_errors" in r else "?"
        print(f"{r['board'][:10]:10} {r['mode']:4} {r['routed']}/{r['connections']:<4} {r['vias']:>4} {add:>5}  {gs}")
    # Gate (doc 05 §38): on the gated boards with the feature on.
    fails = []
    by = {(r["board"], r["mode"]): r for r in rows}
    for tag, _, _, gated in cases:
        if not gated:
            continue
        on, off = by[(tag, "on")], by[(tag, "off")]
        if on["routed"] is None or off["routed"] is None or on["routed"] < off["routed"]:
            fails.append(f"{tag}: completion {on['routed']} below {off['routed']} without buses")
        if "added_errors" in on and on["added_errors"]:
            fails.append(f"{tag}: KiCad errors added: {on['added_errors']}")
        for g in on["buses"]:
            if g["skew_limit_mm"] is not None and not g["within_skew"]:
                fails.append(f"{tag}: bus {g['name']} skew {g['skew_mm']:.3f} mm over {g['skew_limit_mm']:.3f} mm (or not all routed)")
            if g["left"] == 0 and g["vias_min"] != g["vias_max"]:
                fails.append(f"{tag}: bus {g['name']} has {g['vias_min']}-{g['vias_max']} vias per member with no member off the bus")
    print("gate: " + ("PASS" if not fails else "FAIL"))
    for f in fails:
        print("  " + f)
    if a.json:
        pathlib.Path(a.json).write_text(json.dumps({"work": a.work, "rows": rows, "gate_failures": fails}, indent=1))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
