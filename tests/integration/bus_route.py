#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Bus routing (doc 05 §38, D91) on generated boards: grouping, lanes on the same layers, skew, determinism.

  bus_route.py <tracemaker> <work dir>

Generates boards with bench/make_bus_boards.py, routes them with a work budget (no wall-clock dependence) and
judges the written boards with `tracemaker buses`, not with the router's own summary:
  p2p16     --bus with an index range: 16 lanes, each with the one via the back-side IC needs, skew within 1 mm;
  lcd       --bus with a wildcard on four layers: 12 lanes with one via each, skew within 1 mm; without the bus
            options the same board misses the limit, so the feature is what makes the difference;
  skewrule  --buses: the group is found in the net names and takes its limit from the board's .kicad_dru rule;
  spi       three shared nets with four pads each: meanders bring the nets within 2 mm; a member that leaves the
            bus is reported as such by both the router and the measurement.
p2p16 once more with --bus-first. Then the lcd run again at --threads 1, 2 and 4 with the whole portfolio: identical
bytes (rule 2, D47).
Needs no fixtures and no GPU. With kicad-cli present, KiCad's own skew check judges the spi board, whose members
differ in vias: the measurement must count via height as KiCad does.
"""
import json
import pathlib
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parents[2]


def run(cmd, ok=(0,)):
    p = subprocess.run([str(c) for c in cmd], capture_output=True, text=True)
    if p.returncode not in ok:
        print(p.stdout[-3000:], p.stderr[-3000:])
        raise SystemExit(f"FAIL: {' '.join(str(c) for c in cmd[:3])} exited {p.returncode}")
    return p


def main() -> int:
    tm, work = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2])
    work.mkdir(parents=True, exist_ok=True)
    run([sys.executable, ROOT / "bench/make_bus_boards.py", work, "p2p16", "lcd", "skewrule", "spi"])
    manifest = {m["name"]: m for m in json.loads((work / "manifest.json").read_text())}
    fails = []

    def check(cond, text):
        if not cond:
            fails.append(text)

    def route(name, budget, bus_args, tag, extra=("--variants", "1", "--threads", "1")):
        src = work / manifest[name]["board"]
        out = src.with_name(f"{name}.{tag}.kicad_pcb")
        for ext in (".kicad_pro", ".kicad_dru"):  # the rules travel with the board's name
            if src.with_suffix(ext).exists():
                out.with_suffix(ext).write_bytes(src.with_suffix(ext).read_bytes())
        out.unlink(missing_ok=True)
        js = out.with_suffix(".json")
        p = run([tm, "route", src, "-o", out, "--work", budget, "--time", "3600", "--no-kb", "--no-gpu", "--json", js, *extra, *bus_args], ok=(0, 3))
        return out, json.loads(js.read_text()), p

    def measure(board, bus_args):
        js = board.with_suffix(".buses.json")
        p = run([tm, "buses", board, "--json", js, *bus_args], ok=(0, 4))
        return json.loads(js.read_text())["buses"], p.returncode

    budgets = {"p2p16": 6_000_000, "lcd": 5_000_000, "skewrule": 16_000_000, "spi": 30_000_000}
    for name in ("p2p16", "lcd", "skewrule", "spi"):
        args = manifest[name]["bus_args"]
        out, summary, p = route(name, budgets[name], args, "on")
        groups, code = measure(out, args)
        check(p.returncode == 0 and summary["routed"] == summary["connections"], f"{name}: routed {summary['routed']}/{summary['connections']}")
        check(len(groups) == 1, f"{name}: {len(groups)} groups measured, expected 1")
        if len(groups) != 1:
            continue
        g = groups[0]
        print(f"{name}: {g['name']} {g['routed']}/{g['nets']} routed, vias {g['vias_min']}-{g['vias_max']}, left {g['left']}, "
              f"skew {g['skew_mm']:.3f} / {g['skew_limit_mm']} mm ({g['skew_source']}); work {summary['expansions']}")
        check(g["routed"] == g["nets"], f"{name}: {g['routed']}/{g['nets']} members routed")
        check(code == 0 and g["within_skew"] is True, f"{name}: skew {g['skew_mm']:.3f} mm over {g['skew_limit_mm']} mm (buses exit {code})")
        check(g["followed"] + g["left"] == g["routed"], f"{name}: followed + left != routed")
        if name != "spi":  # the fan-out's lanes must cross between the devices: a member may leave, and is reported
            check(g["left"] == 0, f"{name}: {g['left']} members left the bus")
        if g["left"] == 0:
            check(g["vias_min"] == g["vias_max"], f"{name}: {g['vias_min']}-{g['vias_max']} vias per member with nobody off the bus")
        # The route summary reports the same groups with the same numbers as the measurement of the file.
        sg = summary.get("buses", [])
        check(len(sg) == 1 and sg[0]["name"] == g["name"] and [m["net"] for m in sg[0]["members"]] == [m["net"] for m in g["members"]],
              f"{name}: summary and measurement disagree on the group")
        if len(sg) == 1:
            check(abs(sg[0]["skew_mm"] - g["skew_mm"]) < 1e-9 and sg[0]["left"] == g["left"] and sg[0]["vias_max"] == g["vias_max"],
                  f"{name}: summary skew/left/vias differ from the measurement")
        check("bus_lanes" in summary and "bus_left" in summary, f"{name}: no lane counts in the summary")
        if name == "spi":
            # Members with different vias: the lengths compared include the vias' height (1.58 mm through this board).
            check(g["vias_min"] == g["vias_max"] or any(m["via_length_mm"] > 0 for m in g["members"]), "spi: vias add no length")
            check(all(abs(m["via_length_mm"] - 1.58 * m["vias"]) < 1e-6 for m in g["members"]), "spi: a via is not 1.58 mm long")
            if shutil.which("kicad-cli"):
                out.with_suffix(".kicad_dru").write_text(
                    '(version 1)\n(rule "bus_skew"\n\t(constraint skew (max 2mm))\n\t(condition "A.NetName == \'/SPI_*\'"))\n')
                drc = out.with_suffix(".drc.json")
                subprocess.run(["kicad-cli", "pcb", "drc", "--format", "json", "--severity-all", "-o", str(drc), str(out)], capture_output=True)
                skew = [v["description"] for v in json.loads(drc.read_text())["violations"] if v["type"] == "skew_out_of_range"]
                print(f"spi: KiCad reports {len(skew)} skew violation(s) against the same 2 mm limit")
                check(not skew, f"spi: within the limit here, but KiCad: {skew[:1]}")
    # Group definitions.
    out16 = (work / manifest["p2p16"]["board"]).with_name("p2p16.on.kicad_pcb")
    g16, _ = measure(out16, manifest["p2p16"]["bus_args"])
    check([m["net"] for m in g16[0]["members"]] == [f"/D{i}" for i in range(16)], "p2p16: members are not /D0../D15 in index order")
    check(g16[0]["vias_min"] == 1, "p2p16: a lane without the via the back-side IC needs")
    rule, _ = measure((work / manifest["skewrule"]["board"]).with_name("skewrule.on.kicad_pcb"), ["--buses"])
    check(len(rule) == 1 and rule[0]["source"] == "detected" and rule[0]["skew_source"] == "rule" and abs(rule[0]["skew_limit_mm"] - 0.8) < 1e-9,
          "skewrule: the detected group does not carry the 0.8 mm limit of the board's rule")
    # No option, no rule: grouping only, never an invented limit.
    none, code = measure(out16, ["--bus", "DATA=/D[0-15]"])
    check(code == 0 and none[0]["skew_limit_mm"] is None and none[0]["within_skew"] is None, "p2p16: a skew limit appeared without a rule or an option")
    bad = subprocess.run([str(tm), "buses", str(out16), "--bus", "NOPE=/X[0-3]"], capture_output=True, text=True)
    check(bad.returncode == 1 and "matches 0 net" in bad.stderr, "a pattern matching nothing is not an error")
    # --bus-first: the same guarantees with the bus routed before everything else.
    first, sf, pf = route("p2p16", budgets["p2p16"], [*manifest["p2p16"]["bus_args"], "--bus-first"], "first")
    gf, code = measure(first, manifest["p2p16"]["bus_args"])
    print(f"p2p16 --bus-first: vias {gf[0]['vias_min']}-{gf[0]['vias_max']}, left {gf[0]['left']}, skew {gf[0]['skew_mm']:.3f} mm")
    check(pf.returncode == 0 and code == 0 and gf[0]["left"] == 0 and gf[0]["vias_min"] == gf[0]["vias_max"] == 1, "p2p16 --bus-first: not a clean bus")
    # Without the feature the lcd board misses its limit at the same budget.
    off, soff, _ = route("lcd", budgets["lcd"], [], "off")
    goff, code = measure(off, manifest["lcd"]["bus_args"])
    print(f"lcd without buses: skew {goff[0]['skew_mm']:.3f} mm, vias {goff[0]['vias_min']}-{goff[0]['vias_max']}, left {goff[0]['left']}")
    check(code == 4 and goff[0]["within_skew"] is False, "lcd: within the limit without bus routing; the fixture no longer tests anything")
    check("buses" not in soff, "lcd: a buses array in the summary of a run without bus options")
    # Thread determinism of a bus run: the whole portfolio at 1, 2 and 4 threads.
    outs = {}
    for threads in (1, 2, 4):
        out, s, p = route("lcd", 3_000_000, manifest["lcd"]["bus_args"], f"t{threads}", extra=("--threads", str(threads), "--seed", "7"))
        check("portfolio: 8 variants" in p.stdout, f"lcd --threads {threads}: not the whole portfolio")
        outs[threads] = (out.read_bytes(), s["variant"], s["routed"], s["vias"], json.dumps(s["buses"], sort_keys=True))
        print(f"lcd --threads {threads}: winner {s['variant']}, routed {s['routed']}/{s['connections']}, vias {s['vias']}, {len(outs[threads][0])} bytes")
    check(all(o == outs[1] for o in outs.values()), "lcd: output differs between --threads 1, 2 and 4")
    for f in fails:
        print("FAIL: " + f)
    print("PASS" if not fails else f"{len(fails)} failure(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
