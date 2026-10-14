#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Generated boards: native default constraints, project overrides and legal routed widths."""
import json
import pathlib
import shutil
import subprocess
import sys

binary, work = sys.argv[1], pathlib.Path(sys.argv[2])
if not shutil.which("kicad-cli"):
    print("SKIP: kicad-cli unavailable")
    sys.exit(77)
work.mkdir(parents=True, exist_ok=True)


def run(*args, allowed=(0,)):
    result = subprocess.run(args, capture_output=True, text=True, timeout=600)
    if result.returncode not in allowed:
        raise RuntimeError(f"{args}: {result.stdout}\n{result.stderr}")
    return result


def board(extra="", legacy=""):
    return '''(kicad_pcb (version 20211014) (generator pcbnew)
 (layers (0 "F.Cu" signal) (31 "B.Cu" signal) (37 "F.SilkS" user) (44 "Edge.Cuts" user))
 (net 0 "") (net 1 "SIG") (net 2 "OTHER")
 (footprint "Test" (layer "F.Cu") (at 3 5)
  (property "Reference" "J1" (at 0 -1) (layer "F.SilkS"))
  (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SIG")))
 (footprint "Test" (layer "F.Cu") (at 17 5)
  (property "Reference" "J2" (at 0 -1) (layer "F.SilkS"))
  (pad "1" smd rect (at 0 0) (size 1 1) (layers "F.Cu") (net 1 "SIG")))
 (gr_rect (start 0 0) (end 20 10) (layer "Edge.Cuts") (stroke (width 0.1) (type solid)))
''' + legacy + extra + ")\n"


def project_for(path, project):
    # CTest reuses its working directory; a prior run must not supply an absent project's rules.
    path.with_suffix(".kicad_dru").unlink(missing_ok=True)
    pro = path.with_suffix(".kicad_pro")
    if project is None:
        pro.unlink(missing_ok=True)
    else:
        pro.write_text(json.dumps(project))


def violations(path, judge):
    report = work / f"{path.stem}-{judge}.json"
    if judge == "kicad":
        run("kicad-cli", "pcb", "drc", str(path), "--format", "json", "--severity-all", "--all-track-errors", "-o", str(report))
    else:
        run(binary, "drc", str(path), "--json", str(report), allowed=(0, 5))
    return json.loads(report.read_text())["violations"]


def kinds(path, judge):
    return {v["type"] for v in violations(path, judge)}


# Spaced probes expose native violations independently of our reader; do not pin diagnostic prose.
probe_text = board('''
 (segment (start 3 5) (end 17 5) (width 0.15) (layer "F.Cu") (net 1))
 (via (at 4 2) (size 0.4) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
 (via (at 7 2) (size 0.5) (drill 0.2) (layers "F.Cu" "B.Cu") (net 1))
 (via (at 10 2) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
 (via (at 10.5 2) (size 0.6) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
 (segment (start 13 2.2) (end 14 2.2) (width 0.2) (layer "F.Cu") (net 2))
 (via (at 13.5 1.8) (size 0.5) (drill 0.3) (layers "F.Cu" "B.Cu") (net 1))
 (segment (start 0.2 7) (end 0.2 8) (width 0.2) (layer "F.Cu") (net 1))
''')
expected_kinds = {"track_width", "via_diameter", "annular_width", "drill_out_of_range",
                  "hole_to_hole", "hole_clearance", "copper_edge_clearance", "clearance"}
partial = {"board": {"design_settings": {"rules": {"min_clearance": 0}}}}
for name, project in (("no-project-probe", None), ("empty-project-probe", {}), ("partial-probe", partial)):
    probe = work / f"{name}.kicad_pcb"
    probe.write_text(probe_text)
    project_for(probe, project)
    native = kinds(probe, "kicad")
    assert expected_kinds <= native, (name, expected_kinds - native, native)
    engine = kinds(probe, "tracemaker")
    assert {"track_width", "via_diameter"} <= engine, (name, engine)

# Explicit zeros disable the supplied board constraints rather than restoring their defaults.
zero_rules = {key: 0 for key in ("min_track_width", "min_via_diameter", "min_via_annular_width",
                               "min_through_hole_diameter", "min_hole_to_hole", "min_hole_clearance",
                               "min_copper_edge_clearance")}
zero_probe = work / "zero-probe.kicad_pcb"
zero_probe.write_text(probe_text)
project_for(zero_probe, {"board": {"design_settings": {"rules": zero_rules}}})
for judge in ("kicad", "tracemaker"):
    actual = kinds(zero_probe, judge)
    assert not (expected_kinds - {"clearance"}) & actual, ("explicit-zero", judge, actual)

# Legacy settings win even present or zero-valued project board settings during native migration.
thin = '(segment (start 3 5) (end 17 5) (width 0.15) (layer "F.Cu") (net 1))\n'
legacy_min = work / "legacy-min.kicad_pcb"
legacy_min.write_text(board(thin, '(setup (trace_min 0.11))\n'))
for project in (None, partial,
                {"board": {"design_settings": {"rules": {"min_track_width": 0.2}}}},
                {"board": {"design_settings": {"rules": {"min_track_width": 0}}}}):
    project_for(legacy_min, project)
    for judge in ("kicad", "tracemaker"):
        actual = kinds(legacy_min, judge)
        assert "track_width" not in actual, ("legacy-min", project, judge, actual)

# Present minimums override native defaults on boards without legacy design settings.
project_min = work / "project-min.kicad_pcb"
project_min.write_text(board(thin))
for minimum, expected in ((0.12, False), (0.25, True)):
    project_for(project_min, {"board": {"design_settings": {"rules": {"min_track_width": minimum}}}})
    for judge in ("kicad", "tracemaker"):
        actual = kinds(project_min, judge)
        assert ("track_width" in actual) == expected, ("project-min", minimum, judge, actual)

# Cross-property suppression distinguishes the migration flag from a per-key fallback.
# Modern/local setup fields must still allow project board-rule loading.
for name, setup, expected in (
        ("legacy-via-trigger", '(setup (via_min_size 0.45))', False),
        ("legacy-list-trigger", '(setup (user_trace_width 0.4))', False),
        ("local-mask", '(setup (pad_to_mask_clearance 0.1))', True),
        ("local-mask-web", '(setup (solder_mask_min_width 0.07))', True),
        ("unused-last-width", '(setup (last_trace_width 0.4))', True)):
    src = work / f"{name}.kicad_pcb"
    src.write_text(board('(segment (start 3 5) (end 17 5) (width 0.25) (layer "F.Cu") (net 1))', setup))
    project_for(src, {"board": {"design_settings": {"rules": {"min_track_width": 0.3}}}})
    for judge in ("kicad", "tracemaker"):
        actual = kinds(src, judge)
        assert ("track_width" in actual) == expected, (name, judge, actual)

# Legacy classes alone migrate separately: project board minimums still apply.
class_probe = work / "legacy-class-probe.kicad_pcb"
class_probe.write_text(board(thin + '(segment (start 7 5.3) (end 13 5.3) (width 0.15) (layer "F.Cu") (net 2))',
                             '(net_class "Default" "" (clearance 0.05) (trace_width 0.3))'))
project_for(class_probe, {"board": {"design_settings": {"rules": {"min_track_width": 0.2}}},
                          "net_settings": {"classes": [{"name": "Default", "clearance": 0.5}]}})
for judge in ("kicad", "tracemaker"):
    actual = kinds(class_probe, judge)
    assert "track_width" in actual and "clearance" not in actual, ("legacy-class-probe", judge, actual)

# The legacy class is a preferred routing width, not permission to go below the board minimum.
legacy = '(net_class "Default" "" (clearance 0.2) (trace_width 0.15) (via_dia 0.6) (via_drill 0.3) (add_net "SIG"))\n'
missing = {"board": {"design_settings": {"rules": {"min_clearance": 0}}},
           "net_settings": {"classes": [{"name": "Default", "track_width": 0.15}]}}
for name, project, source_class, expected_width in (
        ("no-project", None, legacy, 200_000),
        ("missing-keys", missing, legacy, 200_000),
        ("legacy-class", missing, legacy.replace("(trace_width 0.15)", "(trace_width 0.3)"), 300_000)):
    src = work / f"{name}.kicad_pcb"
    src.write_text(board(legacy=source_class))
    project_for(src, project)
    output, summary = work / f"{name}-routed.kicad_pcb", work / f"{name}-route.json"
    project_for(output, project)
    run(binary, "route", str(src), "-o", str(output), "--work", "1000000", "--time", "3600",
        "--variants", "1", "--threads", "1", "--no-kb", "--no-gpu", "--json", str(summary))
    result = json.loads(summary.read_text())
    assert result["routed"] == result["connections"] == 1, result
    for judge in ("kicad", "tracemaker"):
        assert "track_width" not in kinds(output, judge), (name, judge)
    # Inspect actual written copper, not a requested width echoed in a route summary.
    inspected = work / f"{name}-inspect.json"
    run(binary, "inspect", str(output), "--json", str(inspected))
    tracks = json.loads(inspected.read_text())["tracks"]
    assert tracks and {t["width"] for t in tracks} == {expected_width}, (name, tracks)
print("native defaults, project keys, legacy migration and legal routed widths agree")
