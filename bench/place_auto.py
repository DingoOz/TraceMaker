#!/usr/bin/env python3
"""Evaluate `tracemaker-place --mode auto` on PCBench boards.

For each board: run auto placement with a route check, then report unrouted connections for the input placement
and for the kept placement (same deterministic router budget), and the wirelength change.

  bench/place_auto.py --work 3000000 --jobs 3 Board1 Board2 ...
"""
import argparse
import concurrent.futures as cf
import json
import pathlib
import subprocess

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
PLACE = ROOT / "build/release/src/place/tracemaker-place"


def one(name: str, work: int, out_dir: pathlib.Path, threads: int) -> dict:
    src = FIX / name / "unrouted.kicad_pcb"
    out = out_dir / f"{name}.kicad_pcb"
    js = out_dir / f"{name}.json"
    p = subprocess.run([str(PLACE), str(src), "-o", str(out), "--mode", "auto", "--route-check", str(work), "--route-threads", str(threads),
                        "--threads", str(threads), "--json", str(js)], capture_output=True, text=True)
    kept = next((l.split("kept ")[1].split(" ")[0] for l in p.stdout.splitlines() if l.startswith("auto: kept")), "?")
    try:
        j = json.loads(js.read_text())
        rc = j["route_check"]
        un_in = rc["connections_input"] - rc["routed_input"]
        un_out = un_in if rc["kept_input"] else rc["connections_output"] - rc["routed_output"]
        hb, ha = j["before"]["hpwl_mm"], (j["before"]["hpwl_mm"] if rc["kept_input"] else j["after"]["hpwl_mm"])
        return {"board": name, "mode": kept if not rc["kept_input"] else "input", "unrouted_input": un_in, "unrouted_kept": un_out,
                "hpwl_input": hb, "hpwl_kept": ha}
    except Exception as e:  # noqa: BLE001
        return {"board": name, "error": str(e), "stdout": p.stdout[-300:], "stderr": p.stderr[-300:]}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--work", type=int, default=3_000_000)
    ap.add_argument("--jobs", type=int, default=3)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--out", default=str(ROOT / "build/place-auto"))
    a = ap.parse_args()
    out_dir = pathlib.Path(a.out)
    out_dir.mkdir(parents=True, exist_ok=True)
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(lambda n: one(n, a.work, out_dir, a.threads), a.boards))
    print("| Board | Kept | Unrouted input | Unrouted kept | HPWL input (mm) | HPWL kept (mm) |")
    print("|---|---|--:|--:|--:|--:|")
    better = worse = 0
    for r in rows:
        if "error" in r:
            print(f"| {r['board']} | error: {r['error']} | | | | |")
            continue
        better += r["unrouted_kept"] < r["unrouted_input"]
        worse += r["unrouted_kept"] > r["unrouted_input"]
        print(f"| {r['board']} | {r['mode']} | {r['unrouted_input']} | {r['unrouted_kept']} | {r['hpwl_input']:.0f} | {r['hpwl_kept']:.0f} |")
    print(f"\nFewer unrouted: {better}; more unrouted: {worse}; boards: {len(rows)}")
    (out_dir / "summary.json").write_text(json.dumps(rows, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
