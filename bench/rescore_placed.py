#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Re-judges placement-added errors of finished `run.py --place` runs with the shared native instance gate:
rigid single-footprint defects are not counted; zone-dependent thermal/island errors are. KiCad reports are cached.

  bench/rescore_placed.py placed2-tierA placed2-tierB ...
"""
import json
import pathlib
import sys

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parent))
import run  # noqa: E402


def main() -> int:
    for rid in sys.argv[1:]:
        d = run.ROOT / "bench/results" / rid
        rows = [json.loads(l) for l in (d / "boards.jsonl").read_text().splitlines() if l.strip()]
        changed = 0
        for r in rows:
            placed = d / "boards" / f"{r['board']}.placed.kicad_pcb"
            if r.get("place_exit") != 0 or not placed.exists():
                continue
            human, moved = run.drc(run.FIX / r["board"] / "unrouted.kicad_pcb"), run.drc(placed)
            if not (human and moved):
                r["place_error_gate"] = "unknown"
                r["clean"] = False
                continue
            result = run.drc_judge.added_errors(human, moved, placement=True)
            pa = result["added_errors"]
            if pa != r.get("place_added"):
                print(f"{rid} {r['board']}: place_added {r.get('place_added')} -> {pa}")
                changed += 1
            r["place_added"] = pa
            r["place_error_gate"] = result["error_gate"]
            r["place_capped_evaluated_types"] = result["capped_evaluated_types"]
            r["clean"] = r.get("unconnected_after", 1) == 0 and not r.get("added_errors") and r.get("error_gate", "pass") == "pass" and not pa and result["error_gate"] == "pass"
        (d / "boards.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
        s = json.loads((d / "summary.json").read_text())
        s["clean_pass"] = round(sum(1 for r in rows if r["clean"]) / len(rows), 4)
        s["boards_with_place_added"] = sum(1 for r in rows if r.get("place_added"))
        (d / "summary.json").write_text(json.dumps(s, indent=1))
        print(f"{rid}: {changed} board(s) re-judged, clean {s['clean_pass']:.1%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
