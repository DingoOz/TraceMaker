#!/usr/bin/env python3
"""Roadmap M3 gate: replaying a recorded event log reproduces the final routed board.

  record_replay.py <tracemaker> <workdir>

Routes a small PCBench board with `--record`, applies the recorded track/via add and remove events in order, and
checks that the result equals the tracks and vias of the written board exactly (nanometres). Exit 77 (skipped)
when the fixture is not downloaded.
"""
import collections
import json
import pathlib
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench/C-BISCUIT_buck-reg-5v/unrouted.kicad_pcb"


def main() -> int:
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    if not BOARD.exists():
        print("fixture missing: skipped")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    out, rec, js = work / "replay.kicad_pcb", work / "replay.jsonl", work / "replay.json"
    subprocess.run([tm, "route", str(BOARD), "-o", str(out), "--work", "2000000", "--threads", "4", "--variants", "4", "--no-kb", "--record", str(rec)],
                   check=True, capture_output=True)
    subprocess.run([tm, "inspect", str(out), "--json", str(js)], check=True, capture_output=True)
    board = json.loads(js.read_text())
    layers = board["copper_layers"]
    tracks, vias = {}, {}
    for line in open(rec):
        e = json.loads(line)
        k = e.get("type")
        if k == "track_add":
            tracks[e["track"]["id"]] = e["track"]
        elif k == "track_remove":
            tracks.pop(e["id"], None)
        elif k == "via_add":
            vias[e["via"]["id"]] = e["via"]
        elif k == "via_remove":
            vias.pop(e["id"], None)
    seg = lambda a, b: tuple(sorted([tuple(a), tuple(b)]))  # noqa: E731
    got_t = collections.Counter((seg(t["a"], t["b"]), t["w"], layers[t["layer"]]) for t in tracks.values())
    want_t = collections.Counter((seg((t["sx"], t["sy"]), (t["ex"], t["ey"])), t["width"], t["layer"]) for t in board["tracks"])
    got_v = collections.Counter((tuple(v["p"]), v["d"]) for v in vias.values())
    want_v = collections.Counter(((v["x"], v["y"]), v["size"]) for v in board["vias"])
    ok = got_t == want_t and got_v == want_v
    print(f"replay: {sum(got_t.values())} tracks, {sum(got_v.values())} vias; board: {sum(want_t.values())} tracks, "
          f"{sum(want_v.values())} vias -> {'MATCH' if ok else 'DIFFERENT'}")
    if not ok:
        print("missing from replay:", list((want_t - got_t).items())[:3], "extra:", list((got_t - want_t).items())[:3])
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
