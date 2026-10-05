#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Roadmap M3 gate: replaying a recorded event log reproduces the final routed board.

  record_replay.py <tracemaker> <workdir>

Routes a small PCBench board four times: plainly, with `--record` to a plain `.jsonl` log, with `--record` to a
zstd-compressed `.jsonl.zst` log, and with the live viewer (`--view`, no client). Checks that:
  * all four written boards are byte-identical (recording and viewing never change routing; rules 2 and 7),
  * applying the recorded track/via add and remove events in order gives exactly the tracks and vias of the
    written board (nanometres), for both logs,
  * the two logs carry the same messages, the compressed one is much smaller, and both contain heatmaps.
Exit 77 (skipped) when the fixture is not downloaded.
"""
import collections
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]
BOARD = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench/C-BISCUIT_buck-reg-5v/unrouted.kicad_pcb"


def route(tm, out, *extra):
    subprocess.run([tm, "route", str(BOARD), "-o", str(out), "--work", "2000000", "--threads", "4", "--variants", "4", "--no-kb", *extra],
                   check=True, capture_output=True)


def read_log(path):
    """JSON messages of a recording; .zst is decompressed with the zstd tool (standard zstd frames)."""
    if path.suffix == ".zst":
        if not shutil.which("zstd"):
            return None
        text = subprocess.run(["zstd", "-dc", str(path)], check=True, capture_output=True).stdout.decode()
    else:
        text = path.read_text()
    return [json.loads(line) for line in text.splitlines() if line]


def replay_matches(events, board):
    layers = board["copper_layers"]
    tracks, vias = {}, {}
    for e in events:
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
    return ok


def main() -> int:
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    if not BOARD.exists():
        print("fixture missing: skipped")
        return 77
    work.mkdir(parents=True, exist_ok=True)
    plain, rec_out, zst_out, view_out = (work / f"{n}.kicad_pcb" for n in ("plain", "recorded", "recorded_zst", "viewed"))
    rec, zst, js = work / "replay.jsonl", work / "replay.jsonl.zst", work / "replay.json"
    route(tm, plain)
    route(tm, rec_out, "--record", str(rec))
    route(tm, zst_out, "--record", str(zst))
    route(tm, view_out, "--view", "--view-host", "127.0.0.1", "--view-port", "0")
    ok = True
    base = plain.read_bytes()
    for p in (rec_out, zst_out, view_out):
        same = p.read_bytes() == base
        print(f"{p.name}: {'byte-identical' if same else 'DIFFERENT'} to the plain run")
        ok &= same

    subprocess.run([tm, "inspect", str(rec_out), "--json", str(js)], check=True, capture_output=True)
    board = json.loads(js.read_text())
    events = read_log(rec)
    ok &= replay_matches(events, board)
    heat = collections.Counter(e["name"] for e in events if e.get("type") == "heatmap")
    print(f"heatmaps in the log: {dict(heat)}")
    ok &= heat["expansions"] >= 1 and heat["history"] >= 1

    packed = read_log(zst)
    size_plain, size_zst = rec.stat().st_size, zst.stat().st_size
    print(f"log size: {size_plain} B plain, {size_zst} B zstd ({size_plain / max(1, size_zst):.1f}x smaller)")
    ok &= size_zst * 5 < size_plain
    if packed is None:
        print("zstd tool not found: compressed log content not checked here (tm_server_tests covers the reader)")
    else:
        # Same messages apart from wall-clock seconds (the time stamp and the stats' elapsed_s differ between runs).
        strip = lambda evs: [{k: v for k, v in e.items() if k not in ("t", "elapsed_s")} for e in evs]  # noqa: E731
        same = strip(packed) == strip(events)
        print(f"compressed log: {len(packed)} messages, {'same as' if same else 'DIFFERENT from'} the plain log")
        ok &= same and replay_matches(packed, board)
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
