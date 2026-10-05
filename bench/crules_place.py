#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Placement with and without component-rule proximity pseudo-nets (docs/15-component-rules.md P1).

  bench/crules_place.py [--configs off,soft] [--jobs 2] [--threads 8] [--out build/crules/place] [Board ...]

Runs `tracemaker-place --mode full --seed 1` on each board for each --component-rules setting and reports, per
board and as medians: total HPWL (reported wirelength; pseudo-nets are objective-only and never counted),
airwire crossings, and the design-intent distances of bench/place_intent.py (decoupling caps, crystals, ESD part
to connector, regulator caps, crystal load caps). Default boards: the 23 of docs/04-placement.md §7.3.
"""
import argparse
import concurrent.futures as cf
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
BOARDS = ["1Bitsy_1bitsy", "AzizLight_AzizLight", "ChirpHardware_chirp", "ESP_nRF_Relay_Relay_WiFi_nRF24", "Hardware_Playground_Touch_Switch_2ch_PCB",
          "IGN01A_IGN01A", "LadybugLiteBlue_HW_LadybugBlueLite", "Microdox-PCB_Microdox", "PiPlay_SDHat", "RX5808_rx5808_4button",
          "Solare-BQ24210_Solare-BQ24210", "a123-battery-integration_BCM", "beast-phat_beast-phat", "bullion_bullion", "domotics_out-board",
          "esp32stack_esp32stack", "jadonk_PocketBone", "kitspace_hbridge_driver", "kitspace_training_board_v02", "nanoTracer_nanoTracer",
          "phone_amp_phone_amp", "scimpy_volumebuffer", "uC3Moy_uC3Moy"]
KEYS = ["decap_median_mm", "crystal_median_mm", "esd_median_mm", "regcap_median_mm", "loadcap_median_mm", "xtal_rule_median_mm",
        "conn_edge_median_mm", "conn_edge_mean_mm", "connectors_at_edge", "conn_edge_met"]


def one(board: str, cfg: str, out_dir: pathlib.Path, threads: int, seed: int = 1, reuse: bool = False) -> dict:
    src = FIX / board / "unrouted.kicad_pcb"
    tag = cfg.replace(":--", "_").replace("=", "")
    sub = tag if seed == 1 else f"{tag}-s{seed}"
    out = out_dir / sub / f"{board}.kicad_pcb"
    js = out_dir / sub / f"{board}.json"
    out.parent.mkdir(parents=True, exist_ok=True)
    cmd = [str(PLACE), str(src), "-o", str(out), "--mode", "full", "--seed", str(seed), "--threads", str(threads), "--json", str(js),
           "--component-rules", cfg.split(":")[0]]
    for extra in cfg.split(":")[1:]:  # e.g. "soft:--decap-weight=20"
        cmd += extra.split("=", 1)
    if reuse and js.exists() and out.exists():
        p = subprocess.CompletedProcess(cmd, 0, "", "")
    else:
        p = subprocess.run(cmd, capture_output=True, text=True)
    row = {"board": board, "config": cfg, "exit": p.returncode}
    try:
        j = json.loads(js.read_text())
        row.update({"hpwl_mm": j["after"]["hpwl_mm"], "hpwl_input_mm": j["before"]["hpwl_mm"], "crossings": j["after"]["crossings"],
                    "legal": p.returncode == 0, "moved": j.get("moved"), "seconds": j.get("seconds_total")})
        row.update({k: v for k, v in place_intent.metrics(out, src).items() if k != "board"})
    except Exception as e:  # noqa: BLE001
        row["error"] = f"{e}: {p.stderr[-300:]}"
    return row


def med(v):
    v = [x for x in v if x is not None]
    return round(statistics.median(v), 2) if v else None


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("boards", nargs="*")
    ap.add_argument("--configs", default="off,soft")
    ap.add_argument("--jobs", type=int, default=2)
    ap.add_argument("--threads", type=int, default=8)
    ap.add_argument("--out", default=str(ROOT / "build/crules/place"))
    ap.add_argument("--seed", type=int, default=1)
    ap.add_argument("--reuse", action="store_true", help="reuse existing placements (recompute the metrics only)")
    a = ap.parse_args()
    boards = a.boards or BOARDS
    cfgs = a.configs.split(",")
    out_dir = pathlib.Path(a.out)
    jobs = [(b, c) for b in boards for c in cfgs]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(lambda bc: one(bc[0], bc[1], out_dir, a.threads, a.seed, a.reuse), jobs))
    by = {(r["board"], r["config"]): r for r in rows}
    # Input (human placement) metrics once per board for reference.
    human = {b: place_intent.metrics(FIX / b / "unrouted.kicad_pcb") for b in boards}
    print(f"{'board':42s} " + " ".join(f"{c + ' hpwl':>11s}" for c in cfgs) + "  " + "  ".join(f"{k.replace('_median_mm', '')}({'/'.join(cfgs)})" for k in KEYS))
    for b in boards:
        cells = []
        for c in cfgs:
            r = by[(b, c)]
            cells.append(f"{r.get('hpwl_mm', float('nan')):11.1f}")
        intents = []
        for k in KEYS:
            intents.append("/".join("-" if by[(b, c)].get(k) is None else f"{by[(b, c)][k]:.1f}" for c in cfgs))
        print(f"{b[:42]:42s} " + " ".join(cells) + "  " + "  ".join(f"{x:>12s}" for x in intents))
    summary = {"boards": boards, "configs": cfgs, "rows": rows, "human": human, "median": {}}
    for c in cfgs:
        m = {"hpwl_mm": med([by[(b, c)].get("hpwl_mm") for b in boards]), "crossings": med([by[(b, c)].get("crossings") for b in boards]),
             "illegal": sum(1 for b in boards if not by[(b, c)].get("legal"))}
        for k in KEYS:
            m[k] = med([by[(b, c)].get(k) for b in boards])
        summary["median"][c] = m
    summary["median"]["human"] = {k: med([human[b].get(k) for b in boards]) for k in KEYS}
    summary["hpwl_ratios"] = {}
    for new in cfgs[1:]:
        base = cfgs[0]
        ratios = [by[(b, new)]["hpwl_mm"] / by[(b, base)]["hpwl_mm"] for b in boards if by[(b, base)].get("hpwl_mm") and by[(b, new)].get("hpwl_mm")]
        summary["hpwl_ratios"][new] = {"median": round(statistics.median(ratios), 4) if ratios else None,
                                       "geomean": round(statistics.geometric_mean(ratios), 4) if ratios else None}
        if new == cfgs[1]:
            summary["hpwl_ratio_median"] = summary["hpwl_ratios"][new]["median"]
            summary["hpwl_ratio_geomean"] = summary["hpwl_ratios"][new]["geomean"]
    print("\nmedians:")
    for c, m in summary["median"].items():
        print(f"  {c:6s} " + "  ".join(f"{k}={v}" for k, v in m.items()))
    for new, r in summary["hpwl_ratios"].items():
        print(f"  HPWL {new}/{cfgs[0]}: median ratio {r['median']}, geometric mean {r['geomean']}")
    (out_dir / f"summary-s{a.seed}.json").write_text(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
