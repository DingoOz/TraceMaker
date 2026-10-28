#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Net lengths as TraceMaker's DRC and KiCad's measure them (doc 05 §59).

Each board is copied with a rule that no net can meet, (constraint length (max 1nm)) on every item, so both DRCs
report every length-constrained net with its measured length. The script pairs the reports by net and prints the
differences. KiCad prints lengths to four decimals of a millimetre, so a difference below 0.0001 mm is a match.

    length_parity.py OUT_DIR BOARD.kicad_pcb ... [--engine build/release/src/app/tracemaker]
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
DRU = "(version 1)\n(rule every_net\n   (constraint length (max 0.000001mm)))\n"


def kicad_lengths(path):
    out = {}
    for v in json.loads(path.read_text())["violations"]:
        if v["type"] != "length_out_of_range":
            continue
        m = re.search(r"actual ([-0-9.e]+) mm", v["description"])
        nets = [re.search(r"\[(.*?)\]", i["description"]) for i in v.get("items", [])]
        nets = [n.group(1) for n in nets if n]
        if m and nets:
            out[nets[0]] = float(m.group(1))
    return out


def engine_lengths(path):
    out = {}
    for v in json.loads(path.read_text())["violations"]:
        if v["type"] != "length_out_of_range":
            continue
        m = re.search(r"actual ([-0-9.e]+) mm\) \[(.*)\]$", v["description"])
        if m:
            out[m.group(2).replace("{slash}", "/")] = float(m.group(1))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("out")
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--engine", default=str(ROOT / "build/release/src/app/tracemaker"))
    a = ap.parse_args()
    out = Path(a.out)
    summary = []
    for b in map(Path, a.boards):
        d = out / b.stem
        d.mkdir(parents=True, exist_ok=True)
        pcb = d / b.name
        shutil.copy(b, pcb)
        pro = b.with_suffix(".kicad_pro")
        proj = json.loads(pro.read_text()) if pro.exists() else {"board": {"design_settings": {}}}
        sev = proj.setdefault("board", {}).setdefault("design_settings", {}).setdefault("rule_severities", {})
        sev["length_out_of_range"] = "error"
        (d / pro.name).write_text(json.dumps(proj, indent=2))
        (d / (b.stem + ".kicad_dru")).write_text(DRU)
        subprocess.run([a.engine, "drc", str(pcb), "--json", str(d / "engine.json")], capture_output=True, check=False)
        subprocess.run([str(ROOT / "scripts/kicad-cli"), "pcb", "drc", "--format", "json", "--severity-all", "-o", "kicad.json", pcb.name],
                       cwd=d, capture_output=True, check=False)
        k, e = kicad_lengths(d / "kicad.json"), engine_lengths(d / "engine.json")
        diffs = sorted(((abs(k[n] - e[n]), n, k[n], e[n]) for n in k if n in e), reverse=True)
        off = [x for x in diffs if x[0] > 0.00015]
        row = {"board": b.stem, "kicad_nets": len(k), "engine_nets": len(e), "both": len(diffs),
               "only_kicad": sorted(set(k) - set(e)), "only_engine": sorted(set(e) - set(k)),
               "max_diff_mm": diffs[0][0] if diffs else 0.0, "over_0.0001mm": len(off),
               "worst": [{"net": n, "kicad": kv, "engine": ev} for _, n, kv, ev in off[:10]]}
        summary.append(row)
        print(f"{b.stem}: {len(diffs)} nets in both (KiCad {len(k)}, engine {len(e)}), "
              f"{len(off)} differ by more than 0.0001 mm, largest {row['max_diff_mm']:.4f} mm")
        for w in row["worst"]:
            print(f"   {w['net']}: KiCad {w['kicad']:.4f}, engine {w['engine']:.4f}")
    (out / "lengths.json").write_text(json.dumps(summary, indent=1))
    return 0


if __name__ == "__main__":
    sys.exit(main())
