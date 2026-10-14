#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""JLCPCB's three routing rules through the command line (issue #24, doc 05 section 44):

  critical_nets.py <tracemaker> <workdir>

- the options are on the `tui` screen with their help text, and a scripted run of the screen prints the command that stands for them
- `route --critical-nets SIG --split-cost 20` on a board with a split reference plane: the log names the critical nets, the summary has the per-net report with the
  voids of the plane, the track goes round the split where the same board without the option goes straight across it
- without the options the routed board is byte-identical to a plain route and the summary has no `critical_nets`
- `--si-rules` is the three rules together; a value given beside it wins; `--critical-via-max` warns; a net with no way round a split is reported
"""
import json
import pathlib
import shutil
import subprocess
import sys


def problem():
    pads = [("p0", "U1", -10, 0, "SIG"), ("p1", "U2", 10, 0, "SIG"), ("g0", "U3", -18, 18, "GND"), ("g1", "U4", -16, 18, "GND")]
    obstacles, nets = [], {}
    for pid, part, x, y, net in pads:
        obstacles.append({"type": "rect", "layers": ["top"], "center": {"x": x, "y": y}, "width": 0.6, "height": 0.5, "connectedTo": [pid], "obstacleId": "pad_" + pid, "componentId": part})
        nets.setdefault(net, []).append({"x": x, "y": y, "layer": "top", "pointId": pid})
    connections = [{"name": n, "pointsToConnect": pts, "rootConnectionName": n} for n, pts in nets.items()]
    return {"layerCount": 2, "minTraceWidth": 0.15, "nominalTraceWidth": 0.2, "defaultObstacleMargin": 0.15, "bounds": {"minX": -20, "maxX": 20, "minY": -20, "maxY": 20},
            "obstacles": obstacles, "connections": connections}


def zone(bridge):
    # A ground plane over the whole board (outline and fill) and a copper-pour keepout that makes the slot, so a refill (the audit after a route refills every zone)
    # keeps the split: bridged = the slot is open at the top and the plane is joined along the bottom; otherwise the slot runs the whole height.
    h = 20
    fill = ([[(-h, -h), (h, -h), (h, h), (1, h), (1, -8), (-1, -8), (-1, h), (-h, h)]] if bridge
            else [[(-h, -h), (-1, -h), (-1, h), (-h, h)], [(1, -h), (h, -h), (h, h), (1, h)]])
    outline = " ".join(f"(xy {x} {y})" for x, y in [(-h, -h), (h, -h), (h, h), (-h, h)])
    out = ('(zone (net "GND") (net_name "GND") (layer "B.Cu") (uuid "6f0c3b1a-0000-4000-8000-000000000001") (hatch edge 0.5) (connect_pads (clearance 0.2)) '
           f'(min_thickness 0.2) (fill yes (thermal_gap 0.5) (thermal_bridge_width 0.5)) (polygon (pts {outline}))\n')
    for r in fill:
        out += '(filled_polygon (layer "B.Cu") (pts ' + " ".join(f"(xy {x} {y})" for x, y in r) + "))\n"
    out += ")\n"
    y0 = -8 if bridge else -h
    slot = " ".join(f"(xy {x} {y})" for x, y in [(-1, y0), (1, y0), (1, h), (-1, h)])
    out += ('(zone (net 0) (net_name "") (layer "B.Cu") (uuid "6f0c3b1a-0000-4000-8000-000000000002") (hatch edge 0.5) (connect_pads (clearance 0)) (min_thickness 0.25) '
            '(keepout (tracks allowed) (vias allowed) (pads allowed) (copperpour not_allowed) (footprints allowed)) (fill (thermal_gap 0.5) (thermal_bridge_width 0.5)) '
            f'(polygon (pts {slot})))\n')
    return out


def main():
    tm, work = sys.argv[1], pathlib.Path(sys.argv[2])
    shutil.rmtree(work, ignore_errors=True)
    work.mkdir(parents=True)
    ok = True

    def check(cond, msg):
        nonlocal ok
        if not cond:
            print("FAIL:", msg)
            ok = False

    def run(*args):
        return subprocess.run([tm, *map(str, args)], capture_output=True, text=True, timeout=300)

    (work / "p.json").write_text(json.dumps(problem()))
    boards = {}
    for name, bridge in (("bridged", True), ("split", False)):
        pcb = work / f"{name}.kicad_pcb"
        run("srj-import", work / "p.json", "-o", pcb)
        text = pcb.read_text()
        pcb.write_text(text[: text.rindex(")")] + zone(bridge) + ")\n")
        boards[name] = pcb
    base = ["--work", "300000", "--variants", "1", "--threads", "1", "--no-kb", "--no-gpu"]

    def route(board, out, *extra):
        r = run("route", board, "-o", work / out, "--json", work / (out + ".json"), *base, *extra)
        j = json.loads((work / (out + ".json")).read_text()) if (work / (out + ".json")).exists() else {}
        return r, j

    # 1. Without the options: the plain route, no report. Naming the net without a cost is a report only: the same copper, straight across the slot.
    r0, j0 = route(boards["bridged"], "plain.kicad_pcb")
    check(r0.returncode == 0, f"plain route: exit {r0.returncode}: {r0.stderr[-200:]}")
    check("critical_nets" not in j0, "no critical_nets in the summary unless asked")
    rn, jn = route(boards["bridged"], "named.kicad_pcb", "--critical-nets", "SIG")
    check((work / "named.kicad_pcb").read_bytes() == (work / "plain.kicad_pcb").read_bytes(), "naming a critical net without any cost or order must not change the routed board")
    nn = jn.get("critical_nets", {}).get("nets", [])
    check(nn and nn[0]["void_crossings"] >= 1 and abs(nn[0]["length_mm"] - 20.0) < 0.5, f"straight across the slot, and the report says so: {nn}")
    straight = nn[0]["length_mm"] if nn else 20.0

    # 2. A named critical net with a split cost goes round the slot; the summary says what was found.
    r1, j1 = route(boards["bridged"], "crit.kicad_pcb", "--critical-nets", "SIG", "--split-cost", "20")
    check(r1.returncode == 0, f"critical route: exit {r1.returncode}: {r1.stderr[-200:]}")
    check("critical nets: 1 " in r1.stdout and "tier 1: 1 (SIG)" in r1.stdout, f"the log should name the critical net: {r1.stdout[:400]}")
    check("over a plane void: +20 pitches per lattice step" in r1.stdout, f"the log should say the split cost: {r1.stdout[:600]}")
    cn = j1.get("critical_nets", {})
    nets = cn.get("nets", [])
    check(len(nets) == 1 and nets[0]["name"] == "SIG" and nets[0]["tier"] == 1, f"summary nets: {nets}")
    check(nets and nets[0].get("void_crossings") == 0 and nets[0]["warnings"] == [], f"SIG should run over solid copper all the way: {nets}")
    check(nets and nets[0]["length_mm"] > straight + 5, f"the critical net should go round the slot: {nets and nets[0]['length_mm']} mm vs {straight:.2f}")
    check(cn.get("options", {}).get("split_cost") == 20, f"options in the summary: {cn.get('options')}")

    # 3. A tiny cost is not worth the detour: the crossing is reported.
    r2, j2 = route(boards["bridged"], "cheap.kicad_pcb", "--critical-nets", "SIG", "--split-cost", "0.5")
    nets2 = j2.get("critical_nets", {}).get("nets", [])
    check(nets2 and nets2[0].get("void_crossings", 0) >= 1 and nets2[0]["warnings"], f"the crossing should be reported: {nets2}")
    check("plane void" in r2.stdout and "SIG: runs over" in r2.stdout, f"the log should say it: {r2.stdout[-500:]}")

    # 4. No way round: the net crosses and the report calls it a split; with --split-hard it stays unrouted.
    r3, j3 = route(boards["split"], "nowayround.kicad_pcb", "--critical-nets", "SIG", "--split-cost", "20")
    nets3 = j3.get("critical_nets", {}).get("nets", [])
    check(nets3 and nets3[0].get("splits", 0) >= 1 and any("split" in w for w in nets3[0]["warnings"]), f"a split with no way round should be reported: {nets3}")
    r4, j4 = route(boards["split"], "hard.kicad_pcb", "--critical-nets", "SIG", "--split-hard")
    check(j4.get("routed") == j4.get("connections", 0) - 1, f"--split-hard: SIG stays unrouted, GND routes: {j4.get('routed')}/{j4.get('connections')}")

    # 5. --si-rules is the three rules; a value beside it wins; --critical-via-max warns.
    r5, j5 = route(boards["bridged"], "si.kicad_pcb", "--si-rules", "--critical-nets", "SIG", "--split-cost", "0.5", "--critical-via-max", "0")
    o5 = j5.get("critical_nets", {}).get("options", {})
    check(o5 == {"first": True, "via_mult": 3.0, "split_cost": 0.5, "split_hard": False, "via_max": 0}, f"--si-rules with a split cost of its own: {o5}")
    r6, j6 = route(boards["bridged"], "si2.kicad_pcb", "--si-rules", "--critical-nets", "SIG")
    o6 = j6.get("critical_nets", {}).get("options", {})
    check(o6.get("split_cost") == 4.0 and o6.get("via_mult") == 3.0 and o6.get("first") is True, f"--si-rules defaults: {o6}")

    # 6. The options are on the screen, with their help, and a scripted run prints the command.
    import os
    os.environ["XDG_CONFIG_HOME"] = str(work / "xdg")
    keys = "/--split-cost<enter><enter>7<enter><esc>/--si-rules<enter><space><esc>/--critical-nets<enter><enter>CLK*<enter><esc>"
    rt = run("tui", boards["bridged"], "-o", work / "t.kicad_pcb", "--keys", keys + "p")
    want = f"tracemaker route {boards['bridged']} --output {work / 't.kicad_pcb'} --si-rules --critical-nets 'CLK*' --split-cost 7"
    check(rt.returncode == 0 and rt.stdout.strip() == want, f"the screen should print the command:\n  {rt.stdout.strip()}\nwanted:\n  {want}")
    help_text = run("route", "--help").stdout
    for opt in ("--si-rules", "--critical-nets", "--critical-auto", "--critical-first", "--critical-via-cost", "--critical-via-max", "--split-cost", "--split-hard"):
        check(opt in help_text, f"{opt} should be in route --help")

    print("critical_nets: " + ("ok" if ok else "FAILED"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
