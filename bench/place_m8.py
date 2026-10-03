#!/usr/bin/env python3
"""M8 placement evaluation: human placement vs `--mode auto`, `--mode routable` and `--mode eco` on PCBench boards.

Every placement is judged by the same deterministic router budget (`--work` expansions per portfolio variant,
`--route-threads` variants), so unrouted counts are reproducible. For each board:
  - auto:     tracemaker-place --mode auto     (M7: refine and full, each kept only if not worse than the input)
  - routable: tracemaker-place --mode routable (M8 G: seeds incl. RUDY variants, then penalty rounds + ECO)
  - eco:      tracemaker-place --mode eco      (M8 ECO: small moves near failed connections of the human placement)
plus KiCad DRC (kicad-cli) of every output: new courtyard / hole / edge errors (count increase) and any other new
inter-footprint error (by item pair) relative to the human board, as in src/place/eval_place.py.

  bench/place_m8.py --jobs 4 --threads 4 --route-threads 4 --work 3000000 Board1 Board2 ...
Writes OUT/summary.json, OUT/summary.md and bench/results/<run-id>/summary.json.
"""
import argparse
import concurrent.futures as cf
import datetime
import importlib.util
import json
import pathlib
import shutil
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
PLACE = ROOT / "build/release/src/place/tracemaker-place"

spec = importlib.util.spec_from_file_location("eval_place", ROOT / "src/place/eval_place.py")
ev = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ev)


def place(board_dir: pathlib.Path, src: pathlib.Path, mode: str, a) -> dict:
    out = board_dir / f"{mode}.kicad_pcb"
    js = board_dir / f"{mode}.json"
    t = time.time()
    if not (a.reuse and js.exists()):
        p = subprocess.run([str(PLACE), str(src), "-o", str(out), "--mode", mode, "--route-check", str(a.work), "--route-threads",
                            str(a.route_threads), "--threads", str(a.threads), "--seed", "1", "--json", str(js)],
                           capture_output=True, text=True)
        (board_dir / f"{mode}.log").write_text(p.stdout + "\n" + p.stderr[-4000:])
    secs = time.time() - t
    j = json.loads(js.read_text())
    rc = j["route_check"]
    kept_input = rc["kept_input"]
    un_in = rc["connections_input"] - rc["routed_input"]
    un_out = un_in if kept_input else rc["connections_output"] - rc["routed_output"]
    return {"path": str(out), "unrouted_input": un_in, "unrouted": un_out, "connections": rc["connections_input"] if kept_input else rc["connections_output"],
            "hpwl_input": j["before"]["hpwl_mm"], "hpwl": j["before"]["hpwl_mm"] if kept_input else j["after"]["hpwl_mm"], "kept": j.get("kept", "input" if kept_input else mode),
            "moved": j.get("moved", 0), "legal": j.get("legal", False), "seconds": j.get("seconds_total", secs) if mode != "auto" else secs,
            "routes": j.get("routes"), "eco": j.get("eco")}


def drc_new(human: str, other: str) -> dict:
    hc, _ = ev.drc_counts(human, human.replace(".kicad_pcb", ".drc.json"))
    oc, err = ev.drc_counts(other, other.replace(".kicad_pcb", ".drc.json"))
    if hc is None or oc is None:
        return {"error": err[-300:]}
    diffs = {t: oc.get(t, 0) - hc.get(t, 0) for t in ev.PLACEMENT_TYPES if oc.get(t, 0) > hc.get(t, 0)}
    new = ev.error_keys(other.replace(".kicad_pcb", ".drc.json")) - ev.error_keys(human.replace(".kicad_pcb", ".drc.json"))
    for (t, _), n in new.items():
        if t not in ev.PLACEMENT_TYPES:
            diffs[t] = diffs.get(t, 0) + n
    return diffs


def one(name: str, a) -> dict:
    d = pathlib.Path(a.out) / name
    d.mkdir(parents=True, exist_ok=True)
    src = FIX / name / "unrouted.kicad_pcb"
    human = d / "human.kicad_pcb"
    shutil.copyfile(src, human)
    # Place from the fixture itself (its .kicad_pro carries the design rules) and give every output a copy of the
    # project file, so KiCad's DRC judges all of them with the same rules.
    pro = src.with_suffix(".kicad_pro")
    for k in ["human"] + a.modes:
        if pro.exists():
            shutil.copyfile(pro, d / f"{k}.kicad_pro")
    r = {"board": name}
    for mode in a.modes:
        try:
            r[mode] = place(d, src, mode, a)
            if a.drc:
                r[mode]["new_drc"] = drc_new(str(human), r[mode]["path"])
        except Exception as e:  # noqa: BLE001
            r[mode] = {"error": str(e)}
    return r


def fmt(rows, modes) -> str:
    def drc(x):
        if "new_drc" not in x:
            return "-"
        n = x["new_drc"]
        if "error" in n:
            return "err"
        return "0" if not n else ", ".join(f"{k}+{v}" for k, v in sorted(n.items()))

    head = "| Board | unrouted human | " + " | ".join(f"unrouted {m}" for m in modes) + " | HPWL human | " + " | ".join(f"HPWL {m}" for m in modes) + \
           " | " + " | ".join(f"new DRC {m}" for m in modes) + " | " + " | ".join(f"s {m}" for m in modes) + " | routable kept |"
    lines = [head, "|---|" + "--:|" * (2 + 4 * len(modes)) + "---|"]
    tot = {m: [0, 0.0] for m in modes}
    tot_h = [0, 0.0]
    for r in rows:
        first = next((r[m] for m in modes if "error" not in r.get(m, {"error": 1})), None)
        if first is None:
            lines.append(f"| {r['board']} | error |")
            continue
        tot_h[0] += first["unrouted_input"]
        tot_h[1] += first["hpwl_input"]
        cells = [str(first["unrouted_input"])]
        for m in modes:
            x = r.get(m, {})
            cells.append(str(x["unrouted"]) if "unrouted" in x else "err")
        cells.append(f"{first['hpwl_input']:.0f}")
        for m in modes:
            x = r.get(m, {})
            cells.append(f"{x['hpwl']:.0f}" if "hpwl" in x else "err")
            if "hpwl" in x:
                tot[m][0] += x["unrouted"]
                tot[m][1] += x["hpwl"]
        cells += [drc(r.get(m, {})) for m in modes]
        cells += [f"{r[m]['seconds']:.0f}" if "seconds" in r.get(m, {}) else "-" for m in modes]
        cells.append(r.get("routable", {}).get("kept", "-"))
        lines.append(f"| {r['board']} | " + " | ".join(cells) + " |")
    lines.append(f"| **Total** | {tot_h[0]} | " + " | ".join(str(tot[m][0]) for m in modes) + f" | {tot_h[1]:.0f} | " +
                 " | ".join(f"{tot[m][1]:.0f}" for m in modes) + " |" + " |" * (2 * len(modes) + 1))
    return "\n".join(lines)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--work", type=int, default=3_000_000)
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--threads", type=int, default=4)
    ap.add_argument("--route-threads", type=int, default=4)
    ap.add_argument("--modes", default="auto,routable,eco")
    ap.add_argument("--out", default=str(ROOT / "build/place-m8"))
    ap.add_argument("--no-drc", dest="drc", action="store_false")
    ap.add_argument("--reuse", action="store_true")
    a = ap.parse_args()
    a.modes = a.modes.split(",")
    pathlib.Path(a.out).mkdir(parents=True, exist_ok=True)
    t0 = time.time()
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(lambda n: one(n, a), a.boards))
    table = fmt(rows, a.modes)
    (pathlib.Path(a.out) / "summary.json").write_text(json.dumps(rows, indent=1))
    (pathlib.Path(a.out) / "summary.md").write_text(table + "\n")
    print(table)
    # Dashboard entry: "clean" = every connection routed with no new DRC error (routable mode).
    m = "routable" if "routable" in a.modes else a.modes[0]
    ok = [r for r in rows if "unrouted" in r.get(m, {})]
    clean = sum(1 for r in ok if r[m]["unrouted"] == 0 and not r[m].get("new_drc"))
    conns = sum(r[m]["connections"] for r in ok)
    run_id = "place-m8-" + datetime.datetime.now().strftime("%Y%m%d-%H%M")
    res = ROOT / "bench/results" / run_id
    res.mkdir(parents=True, exist_ok=True)
    (res / "summary.json").write_text(json.dumps({
        "set": f"PCBench placement ({len(a.boards)} boards, {m} mode, router {a.work} expansions x {a.route_threads})",
        "boards": len(ok), "clean_pass": clean,
        "completion": (conns - sum(r[m]["unrouted"] for r in ok)) / conns if conns else 0.0,
        "seconds": time.time() - t0, "rows": rows}, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
