#!/usr/bin/env python3
"""Side assignment (D48): placement with and without --flip on the 23 boards of docs/04-placement.md §7.3.

  bench/place_flip.py [--modes full,refine] [--jobs 2] [--threads 4] [--out build/flip] [--no-drc] [--base BIN] [Board ...]

Runs `tracemaker-place --mode M --seed 1` per board with and without --flip and reports total HPWL (all nets, mm),
airwire crossings, parts flipped, the via estimate of the side assignment (nets' surface-mount pins on their
minority side), legality as the placer sees it, the decoupling-capacitor distance of bench/place_intent.py, and
KiCad 10 DRC errors the placement added relative to the human board (bench/place_m8.py drc_new: courtyard, hole,
edge counts plus any new inter-footprint error). `--base BIN` also runs an older tracemaker-place without --flip and
checks that the flip-off output is byte-identical (the option must not change anything when it is off).
"""
import argparse
import concurrent.futures as cf
import filecmp
import importlib.util
import json
import pathlib
import statistics
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "bench"))
import place_intent  # noqa: E402

FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
PLACE = ROOT / "build/release/src/place/tracemaker-place"
spec = importlib.util.spec_from_file_location("place_m8", ROOT / "bench/place_m8.py")
m8 = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m8)
BOARDS = m8.BOARDS if hasattr(m8, "BOARDS") else None
if not BOARDS:
    spec2 = importlib.util.spec_from_file_location("crules_place", ROOT / "bench/crules_place.py")
    cp = importlib.util.module_from_spec(spec2)
    spec2.loader.exec_module(cp)
    BOARDS = cp.BOARDS


def run(board: str, mode: str, flip: bool, out_dir: pathlib.Path, threads: int, binary: pathlib.Path = PLACE, tag: str = "") -> dict:
    src = FIX / board / "unrouted.kicad_pcb"
    sub = out_dir / f"{mode}-{'flip' if flip else 'off'}{tag}"
    sub.mkdir(parents=True, exist_ok=True)
    out, js = sub / f"{board}.kicad_pcb", sub / f"{board}.json"
    cmd = [str(binary), str(src), "-o", str(out), "--mode", mode, "--seed", "1", "--threads", str(threads), "--json", str(js)]
    if flip:
        cmd.append("--flip")
    p = subprocess.run(cmd, capture_output=True, text=True)
    row = {"exit": p.returncode, "path": str(out)}
    try:
        j = json.loads(js.read_text())
        row.update({"hpwl": j["after"]["hpwl_mm"], "hpwl_in": j["before"]["hpwl_mm"], "cross": j["after"]["crossings"],
                    "cross_in": j["before"]["crossings"], "flipped": j["after"].get("flipped", 0), "vias": j["after"].get("side_vias", 0),
                    "vias_in": j["before"].get("side_vias", 0), "legal": j.get("legal", False), "seconds": j.get("seconds_total"),
                    "movable": j.get("movable")})
        try:
            row["decap_mm"] = place_intent.metrics(out, src).get("decap_median_mm")
        except Exception:  # noqa: BLE001
            row["decap_mm"] = None
    except Exception as e:  # noqa: BLE001
        row["error"] = f"{e}: {p.stderr[-300:]}"
    return row


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("boards", nargs="*")
    ap.add_argument("--modes", default="full,refine")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--out", default=str(ROOT / "build/flip"))
    ap.add_argument("--no-drc", dest="drc", action="store_false")
    ap.add_argument("--base", help="older tracemaker-place: flip-off outputs must be byte-identical to it")
    a = ap.parse_args()
    boards = a.boards or BOARDS
    out_dir = pathlib.Path(a.out)
    human_dir = out_dir / "human"
    human_dir.mkdir(parents=True, exist_ok=True)
    modes = a.modes.split(",")
    jobs = [(b, m, f) for b in boards for m in modes for f in (False, True)]

    def one(job):
        b, m, f = job
        r = run(b, m, f, out_dir, a.threads)
        if a.base and not f:
            br = run(b, m, False, out_dir, a.threads, pathlib.Path(a.base), "-base")
            r["identical_to_base"] = br.get("exit") == r.get("exit") and filecmp.cmp(br["path"], r["path"], shallow=False)
        if a.drc and "error" not in r:
            human = human_dir / f"{b}.kicad_pcb"
            if not human.exists():
                human.write_bytes((FIX / b / "unrouted.kicad_pcb").read_bytes())
            r["new_drc"] = m8.drc_new(str(human), r["path"])
        return b, m, f, r

    # Human DRC first, once per board (avoids two jobs racing on the same file).
    if a.drc:
        def hum(b):
            human = human_dir / f"{b}.kicad_pcb"
            human.write_bytes((FIX / b / "unrouted.kicad_pcb").read_bytes())
            m8.ev.drc_counts(str(human), str(human).replace(".kicad_pcb", ".drc.json"))
        with cf.ThreadPoolExecutor(a.jobs) as ex:
            list(ex.map(hum, boards))
    rows = {}
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        for b, m, f, r in ex.map(one, jobs):
            rows.setdefault(b, {}).setdefault(m, {})["flip" if f else "off"] = r
            print(f"{b} {m} {'flip' if f else 'off'}: {json.dumps({k: v for k, v in r.items() if k != 'path'})}", flush=True)
    (out_dir / "summary.json").write_text(json.dumps(rows, indent=1))

    def cell(x, k, fmt="{:.0f}"):
        v = x.get(k)
        return "–" if v is None else fmt.format(v)

    def drc(x):
        if "new_drc" not in x:
            return "–"
        n = x["new_drc"]
        return "err" if "error" in n else str(sum(n.values())) if n else "0"

    for m in modes:
        print(f"\n### {m}\n")
        print("| Board | movable | HPWL off | HPWL flip | crossings off / flip | flipped | via est. off / flip | decap mm off / flip | legal off / flip | new DRC off / flip |")
        print("|---|--:|--:|--:|---|--:|---|---|---|---|")
        tot = {"off": 0.0, "flip": 0.0}
        ratios, cx = [], {"off": 0, "flip": 0}
        for b in boards:
            o, f = rows[b][m]["off"], rows[b][m]["flip"]
            if "hpwl" in o and "hpwl" in f:
                tot["off"] += o["hpwl"]
                tot["flip"] += f["hpwl"]
                cx["off"] += o["cross"]
                cx["flip"] += f["cross"]
                if o["hpwl"] > 0:
                    ratios.append(f["hpwl"] / o["hpwl"])
            print(f"| {b} | {cell(o, 'movable')} | {cell(o, 'hpwl')} | {cell(f, 'hpwl')} | {cell(o, 'cross')} / {cell(f, 'cross')} | {cell(f, 'flipped')} | "
                  f"{cell(o, 'vias')} / {cell(f, 'vias')} | {cell(o, 'decap_mm', '{:.1f}')} / {cell(f, 'decap_mm', '{:.1f}')} | "
                  f"{'yes' if o.get('legal') else 'NO'} / {'yes' if f.get('legal') else 'NO'} | {drc(o)} / {drc(f)} |")
        print(f"\nTotal HPWL off {tot['off']:.0f} mm, flip {tot['flip']:.0f} mm; median ratio flip/off "
              f"{statistics.median(ratios) if ratios else 0:.3f}; crossings off {cx['off']}, flip {cx['flip']}")
        if a.base:
            same = sum(1 for b in boards if rows[b][m]["off"].get("identical_to_base"))
            print(f"flip off byte-identical to base: {same}/{len(boards)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
