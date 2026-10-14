#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""After-route passes (doc 05 §39), end to end through the command line on two generated boards.

  after_route_passes.py <tracemaker> <work dir>

Style metric: the route summary carries a `style` object; it is the same at 1 and 3 threads for the same work
budget, and `tracemaker style` on the written board gives the same numbers.
Power widening (--widen-power, --widen-net): a power track in a channel between two other nets takes the target
width when it fits and a smaller step when it does not; a locked track of the same net that was on the board keeps
its width; without the option nothing changes; the in-engine DRC finds no new violation.
Stitching vias (--stitch-vias, --stitch-return): two overlapping GND planes get grid vias written as free vias,
the signal's layer-change via gets a GND via within the distance, the result is the same on a second run, and the
DRC after a refill (in the engine, and KiCad's when kicad-cli is on the PATH) finds no new violation, no
additional unconnected item and no dangling via.
"""
import json
import pathlib
import shutil
import subprocess
import sys

HEAD = """(kicad_pcb (version 20240108) (generator "pcbnew")
  (general (thickness 1.6))
  (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (44 "Edge.Cuts" user))
  (setup (pad_to_mask_clearance 0))
  (net 0 "") (net 1 "GND") (net 2 "P") (net 3 "S") (net 4 "W1") (net 5 "W2")
  (gr_rect (start 0 0) (end 30 20) (stroke (width 0.1) (type default)) (fill none) (layer "Edge.Cuts"))
"""


def pad(ref, x, y, net, name, layer="F.Cu", size=2):
    return (f'  (footprint "T:P" (layer "{layer}") (at {x} {y}) (property "Reference" "{ref}")\n'
            f'    (pad "1" smd rect (at 0 0) (size {size} {size}) (layers "{layer}") (net {net} "{name}")))\n')


def seg(x0, y0, x1, y1, net, width=0.2, locked=False):
    return f'  (segment (start {x0} {y0}) (end {x1} {y1}) (width {width}){" (locked yes)" if locked else ""} (layer "F.Cu") (net {net}))\n'


def zone(layer):
    return (f'  (zone (net 1) (net_name "GND") (layer "{layer}") (uuid "z{layer}") (hatch edge 0.5) (connect_pads yes (clearance 0.2))\n'
            '    (min_thickness 0.25) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5))\n'
            '    (polygon (pts (xy 1 1) (xy 29 1) (xy 29 19) (xy 1 19))))\n')


# P from pad to pad along y = 10 through a channel of two locked wall tracks (nets of their own), 0.9 mm of room;
# a locked P stub on the first pad.
WIDEN = (HEAD + pad("R1", 5, 10, 2, "P") + pad("R2", 25, 10, 2, "P") + seg(8, 9.25, 22, 9.25, 4, locked=True) + seg(8, 10.75, 22, 10.75, 5, locked=True) +
         seg(5, 10, 5, 13, 2, width=0.2, locked=True) + ")\n")
# GND planes on both layers tied by one plated pin; S must change layer between its two pads.
STITCH = (HEAD + '  (footprint "T:H" (layer "F.Cu") (at 3 17) (property "Reference" "J1")\n'
          '    (pad "1" thru_hole circle (at 0 0) (size 1.6 1.6) (drill 0.8) (layers "*.Cu") (net 1 "GND")))\n' +
          pad("R3", 10, 5, 3, "S", size=1) + pad("R4", 20, 5, 3, "S", "B.Cu", size=1) + zone("F.Cu") + zone("B.Cu") + ")\n")

errors = []


def check(cond, text):
    if not cond:
        errors.append(text)


def route(tm, src, out, extra, threads=1, variants=1):
    r = subprocess.run([str(tm), "route", str(src), "-o", str(out), "--work", "200000", "--time", "3600", "--variants", str(variants), "--threads", str(threads),
                        "--no-kb", "--no-gpu", "--json", str(out) + ".json"] + extra, capture_output=True, text=True)
    if r.returncode not in (0, 3):
        sys.exit(f"{out.name}: tracemaker failed\n{r.stdout[-2000:]}\n{r.stderr[-2000:]}")
    return json.loads(pathlib.Path(str(out) + ".json").read_text()), r.stdout


def inspect(tm, board):
    js = board.with_suffix(".truth.json")
    subprocess.run([str(tm), "inspect", str(board), "--json", str(js)], check=True, capture_output=True)
    return json.loads(js.read_text())


def drc(tm, board, refill=False):
    js = board.with_suffix(".drc.json")
    subprocess.run([str(tm), "drc", str(board), "--json", str(js)] + (["--refill-zones"] if refill else []), capture_output=True)
    d = json.loads(js.read_text())
    return sorted(v["type"] for v in d["violations"] if v["severity"] == "error"), len(d["unconnected_items"])


def kicad(board):
    js = board.with_suffix(".kicad.json")
    subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "--refill-zones", "-o", str(js), str(board)],
                   capture_output=True, timeout=600)
    d = json.loads(js.read_text())
    return sorted(v["type"] for v in d["violations"] if not v["type"].startswith(("lib_", "silk_"))), len(d["unconnected_items"])


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    wsrc, ssrc = work / "widen_in.kicad_pcb", work / "stitch_in.kicad_pcb"
    wsrc.write_text(WIDEN)
    ssrc.write_text(STITCH)

    # --- style metric
    s1, _ = route(tm, ssrc, work / "style1.kicad_pcb", [], threads=1, variants=3)
    s3, _ = route(tm, ssrc, work / "style3.kicad_pcb", [], threads=3, variants=3)
    check("style" in s1 and s1["style"]["connections"] > 0, f"no style object in the summary: {s1.get('style')}")
    check(s1.get("style") == s3.get("style"), f"style differs between 1 and 3 threads: {s1.get('style')} vs {s3.get('style')}")
    check((work / "style1.kicad_pcb").read_bytes() == (work / "style3.kicad_pcb").read_bytes(), "boards differ between 1 and 3 threads")
    sj = work / "style1.style.json"
    subprocess.run([str(tm), "style", str(work / "style1.kicad_pcb"), "--json", str(sj), "--connections"], check=True, capture_output=True)
    sub = json.loads(sj.read_text())
    rows = sub.pop("per_connection", None)
    check(sub == s1.get("style"), f"`tracemaker style` on the output differs from the summary: {sub} vs {s1.get('style')}")
    check(isinstance(rows, list) and len(rows) == sub["connections"], "--connections: one row per connection expected")

    # --- power widening
    off, _ = route(tm, wsrc, work / "widen_off.kicad_pcb", [])
    check("widen" not in off and "stitch" not in off, "pass summaries present without the options")
    check(off["routed"] == off["connections"] == 1, f"widen board: routed {off['routed']}/{off['connections']}")
    b_off = inspect(tm, work / "widen_off.kicad_pcb")

    def new_p(b):
        return [t for t in b["tracks"] if t["net"] == "P" and not (t["sx"] == t["ex"] == 5_000_000)]

    w0 = {t["width"] for t in new_p(b_off)}
    check(len(w0) == 1, f"widen board: one class width expected, got {w0}")
    on, log = route(tm, wsrc, work / "widen_on.kicad_pcb", ["--widen-power", "--widen-net", "P=0.8"])
    b_on = inspect(tm, work / "widen_on.kicad_pcb")
    check({t["width"] for t in new_p(b_on)} == {800_000}, f"P=0.8: widths {sorted({t['width'] for t in new_p(b_on)})}, expected 0.8 mm everywhere")
    stub = [t for t in b_on["tracks"] if t["net"] == "P" and t["sx"] == t["ex"] == 5_000_000]
    check(len(stub) == 1 and stub[0]["width"] == 200_000, f"the locked P stub changed: {stub}")
    check([t["width"] for t in b_on["tracks"] if t["net"] in ("W1", "W2")] == [200_000, 200_000], "the wall tracks changed")
    w = on.get("widen", {})
    net = (w.get("nets") or [{}])[0]
    check(w.get("kept") is True and w.get("tracks_widened", 0) >= 1 and net.get("net") == "P" and net.get("target_mm") == 0.8 and net.get("source") == "option",
          f"widen summary: {w}")
    check(abs(sum(net.get("before_mm_by_width", {}).values()) - sum(net.get("after_mm_by_width", {}).values())) < 1e-6 and
          list(net.get("after_mm_by_width", {})) == ["0.800"], f"widen lengths by width: {net}")
    check("widen P (target 0.800 mm, option)" in log, "no log line for the widened net")
    check(drc(tm, work / "widen_on.kicad_pcb") == drc(tm, work / "widen_off.kicad_pcb"), "widening changed the DRC result")
    wide, _ = route(tm, wsrc, work / "widen_wide.kicad_pcb", ["--widen-power", "--widen-net", "P=1.2"])
    b_wide = inspect(tm, work / "widen_wide.kicad_pcb")
    in_channel = [t["width"] for t in new_p(b_wide) if min(t["sx"], t["ex"]) < 22_000_000 and max(t["sx"], t["ex"]) > 8_000_000]
    check(in_channel and all(min(w0) < x <= 900_000 for x in in_channel), f"P=1.2: channel widths {in_channel}, expected above {w0} and at most 0.9 mm")
    check(drc(tm, work / "widen_wide.kicad_pcb") == drc(tm, work / "widen_off.kicad_pcb"), "partial widening changed the DRC result")
    r = subprocess.run([str(tm), "route", str(wsrc), "-o", str(work / "bad.kicad_pcb"), "--widen-power", "--widen-net", "P=wide"], capture_output=True, text=True)
    check(r.returncode not in (0, 3), "--widen-net with a bad width was accepted")

    # --- stitching vias
    soff, _ = route(tm, ssrc, work / "stitch_off.kicad_pcb", [])
    opts = ["--stitch-vias", "all", "--stitch-pitch", "5", "--stitch-distance", "2", "--stitch-return", "S=GND"]
    son, log = route(tm, ssrc, work / "stitch_on.kicad_pcb", opts)
    son2, _ = route(tm, ssrc, work / "stitch_on2.kicad_pcb", opts)
    check((work / "stitch_on.kicad_pcb").read_bytes() == (work / "stitch_on2.kicad_pcb").read_bytes(), "stitching is not deterministic")
    check(son["routed"] == soff["routed"] == soff["connections"], f"stitch board: routed {soff['routed']}/{soff['connections']} and {son['routed']}")
    st = son.get("stitch", {})
    b_s = inspect(tm, work / "stitch_on.kicad_pcb")
    gnd = [v for v in b_s["vias"] if v["net"] == "GND"]
    sig = [v for v in b_s["vias"] if v["net"] == "S"]
    check(st.get("kept") is True and st.get("plane_vias", 0) >= 15, f"stitch summary: {st}")
    check(len(gnd) == st.get("plane_vias", 0) + st.get("return_path_vias", 0) == son["vias"] - soff["vias"], f"{len(gnd)} GND vias on the board, summary {st}")
    check((work / "stitch_on.kicad_pcb").read_text().count("(free yes)") == len(gnd), "stitching vias are not written as free vias")
    check(len(sig) >= 1, "the signal did not change layer: the board no longer tests return-path stitching")
    rp = (st.get("return_paths") or [{}])[0]
    check(rp.get("net") == "S" and rp.get("plane_net") == "GND" and rp.get("signal_vias") == len(sig) and rp.get("unstitched_after") == 0, f"return path: {rp}")
    for v in sig:
        near = min(((v["x"] - g["x"]) ** 2 + (v["y"] - g["y"]) ** 2) ** 0.5 for g in gnd) if gnd else 1e18
        check(near <= 2_000_000, f"signal via at ({v['x']}, {v['y']}): nearest GND via {near / 1e6:.2f} mm away")
    check("stitch vias:" in log and "return path S over GND" in log, "no log lines for the stitching pass")
    e_off, e_on = drc(tm, work / "stitch_off.kicad_pcb", True), drc(tm, work / "stitch_on.kicad_pcb", True)
    check(e_on[0] == e_off[0] and e_on[1] <= e_off[1], f"stitching changed the engine's DRC after a refill: {e_off} -> {e_on}")
    if shutil.which("kicad-cli"):
        k_off, k_on = kicad(work / "stitch_off.kicad_pcb"), kicad(work / "stitch_on.kicad_pcb")
        check(k_on[0] == k_off[0] and k_on[1] <= k_off[1], f"KiCad's DRC after a refill: {k_off} -> {k_on}")
        check("via_dangling" not in k_on[0], "KiCad reports a dangling via")
    else:
        print("kicad-cli not found: KiCad's verdict on the stitched board skipped")

    for e in errors:
        print("FAIL:", e)
    print("after-route passes: " + ("ok" if not errors else f"{len(errors)} failures"))
    return 1 if errors else 0


if __name__ == "__main__":
    sys.exit(main())
