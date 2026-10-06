#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Judges the designers' own routed boards (PCBench raw.kicad_pcb) the way bench/run.py judges TraceMaker's output.

  bench/human_baseline.py RUN [RUN ...] [--jobs 8]      e.g. bench/human_baseline.py plangap-tierB plangap-tierC

For every board of the given result runs: KiCad DRC of raw.kicad_pcb under the fixture's rules (PCBench ships no
project file, so KiCad's defaults apply), errors on routed copper, unconnected items, and the zones the designer
used that the unrouted fixture no longer has. A board whose human layout fails the same judge is not a fair
"clean pass" target; the table says how many of TraceMaker's non-clean boards are of that kind (doc 05 §18).
"""
import argparse
import concurrent.futures as cf
import json
import pathlib
import re

import run as bench

ROOT = pathlib.Path(__file__).resolve().parent.parent


def zones(path: pathlib.Path) -> int:
    return len(re.findall(r"^\s*\(zone\b", path.read_text(errors="replace"), flags=re.M))


def one(rec: dict) -> dict:
    d = bench.FIX / rec["board"]
    raw = d / "raw.kicad_pcb"
    out = {"board": rec["board"], "tier": rec.get("tier"), "tm_clean": rec["clean"], "tm_unrouted": rec["connections"] - rec["routed"],
           "fr_clean": (rec.get("fr_rc12") or {}).get("clean")}
    if not raw.exists():
        return out | {"raw": "missing"}
    j = bench.drc(raw)
    if j is None:
        return out | {"raw": "drc failed"}
    routed = sum(j["routed_errors"].values())
    return out | {"raw": "ok", "raw_routed_errors": routed, "raw_error_kinds": j["routed_errors"], "raw_unconnected": j["unconnected"],
                  "raw_zones": zones(raw), "unrouted_zones": zones(d / "unrouted.kicad_pcb"),
                  "raw_clean": routed == 0 and j["unconnected"] == 0}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("runs", nargs="+")
    ap.add_argument("--jobs", type=int, default=8)
    ap.add_argument("--out", default=str(ROOT / "bench/results/human-baseline.json"))
    a = ap.parse_args()
    recs = [json.loads(line) for r in a.runs for line in (ROOT / "bench/results" / r / "boards.jsonl").read_text().splitlines()]
    with cf.ThreadPoolExecutor(a.jobs) as ex:
        rows = list(ex.map(one, recs))
    pathlib.Path(a.out).write_text(json.dumps(rows, indent=1))
    ok = [r for r in rows if r["raw"] == "ok"]
    print(f"boards {len(rows)}, human layout judged {len(ok)}, human clean {sum(r['raw_clean'] for r in ok)}")
    for tier in sorted({r["tier"] for r in ok}):
        t = [r for r in ok if r["tier"] == tier]
        nc = [r for r in t if not r["tm_clean"]]
        print(f"tier {tier}: {len(t)} boards, human clean {sum(r['raw_clean'] for r in t)}, TraceMaker clean {sum(r['tm_clean'] for r in t)}; "
              f"of TraceMaker's {len(nc)} non-clean: human also not clean {sum(not r['raw_clean'] for r in nc)}, "
              f"human has routed-copper errors {sum(r['raw_routed_errors'] > 0 for r in nc)}, "
              f"designer used zones the fixture lacks {sum(r['raw_zones'] > r['unrouted_zones'] for r in nc)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
