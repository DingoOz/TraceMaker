#!/usr/bin/env python3
"""Re-judges placement-added errors of finished `run.py --place` runs with the current rule (run.place_added):
violations inside one footprint are not counted. KiCad reports are cached, so this is quick.

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
                continue
            pa = run.place_added(human, moved)
            if pa != r.get("place_added"):
                print(f"{rid} {r['board']}: place_added {r.get('place_added')} -> {pa}")
                changed += 1
            r["place_added"] = pa
            r["clean"] = r.get("unconnected_after", 1) == 0 and not r.get("added_errors") and not pa
        (d / "boards.jsonl").write_text("".join(json.dumps(r) + "\n" for r in rows))
        s = json.loads((d / "summary.json").read_text())
        s["clean_pass"] = round(sum(1 for r in rows if r["clean"]) / len(rows), 4)
        s["boards_with_place_added"] = sum(1 for r in rows if r.get("place_added"))
        (d / "summary.json").write_text(json.dumps(s, indent=1))
        print(f"{rid}: {changed} board(s) re-judged, clean {s['clean_pass']:.1%}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
