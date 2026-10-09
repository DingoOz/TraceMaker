#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare refilled KiCad DRC reports without modifying either board."""

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import shlex
import shutil
import subprocess
import sys
import tempfile


NOT_ROUTING = {
    "lib_footprint_issues", "lib_footprint_mismatch", "silk_overlap", "silk_over_copper",
    "silk_edge_clearance", "text_height", "text_thickness", "courtyards_overlap",
    "missing_courtyard", "malformed_courtyard", "footprint_type_mismatch",
    "footprint_filters_mismatch", "nonmirrored_text_on_back_layer", "npth_inside_courtyard",
    "pth_inside_courtyard", "duplicate_footprints", "extra_footprint", "missing_footprint",
    "footprint_symbol_mismatch", "unconnected_items", "track_dangling", "via_dangling",
    "holes_co_located",
}


def flat(text):
    """One printable line: item descriptions carry net and footprint names, the board author's text."""
    return " ".join("".join(c if c.isprintable() else " " for c in str(text)).split())


def routing_error(v):
    if v.get("severity") != "error" or v.get("type") in NOT_ROUTING:
        return False
    kind = v["type"]
    descriptions = [i.get("description", "") for i in v.get("items", [])]
    return (kind in {"starved_thermal", "isolated_copper"}
            or any(d.startswith(("Track", "Via", "Arc")) for d in descriptions)
            or kind in {"clearance", "shorting_items"}
            and any(d.startswith("Zone") for d in descriptions))


def drc(cli, board, out):
    command = [cli, "pcb", "drc", "--refill-zones", "--format", "json",
               "--severity-all", "--all-track-errors", "-o", str(out), str(board)]
    # stdin is closed: a containerised kicad-cli (docker run -i) would otherwise read the caller's input.
    p = subprocess.run(command, stdin=subprocess.DEVNULL, capture_output=True, text=True,
                       timeout=1800)  # large boards refill for minutes
    if p.returncode not in (0, 5) or not out.is_file():
        raise RuntimeError(f"KiCad DRC failed for {board} (exit {p.returncode}): "
                           + (p.stderr or p.stdout).strip())
    report = json.loads(out.read_text())
    if not isinstance(report.get("violations"), list) or not isinstance(report.get("unconnected_items"), list):
        raise ValueError(f"Invalid KiCad DRC JSON for {board}")
    return report


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("input", type=Path)
    parser.add_argument("routed", type=Path)
    parser.add_argument("--route-json", type=Path)
    parser.add_argument("--json", type=Path)
    parser.add_argument("--stage-dir", type=Path,
                        help="where the temporary copies go (default: the system temporary directory); give a directory "
                             "kicad-cli can read when it runs in a container or sandbox (Docker, Flatpak, Snap)")
    args = parser.parse_args()
    if not (str(args.input).isprintable() and str(args.routed).isprintable()):
        parser.error("board paths contain control characters; rename the files")
    result = {"input": str(args.input), "routed": str(args.routed), "warnings": []}
    try:
        cli = shutil.which(os.environ.get("KICAD_CLI", "kicad-cli"))
        if not cli:
            raise RuntimeError("kicad-cli not found; set KICAD_CLI or add it to PATH")
        for board in (args.input, args.routed):
            if not board.is_file():
                raise FileNotFoundError(board)
        if not args.input.with_suffix(".kicad_pro").is_file():
            result["warnings"].append(f"No sibling .kicad_pro for {args.input}; KiCad judges both boards on default net classes "
                                      "and minimums. Keep the board beside its project files.")
        help_run = subprocess.run([cli, "pcb", "drc", "--help"], stdin=subprocess.DEVNULL, capture_output=True,
                                  text=True, timeout=60)
        save_board = help_run.returncode == 0 and "--save-board" in help_run.stdout + help_run.stderr
        if args.stage_dir:
            args.stage_dir.mkdir(parents=True, exist_ok=True)
        with tempfile.TemporaryDirectory(prefix="tracemaker-signoff-", dir=args.stage_dir) as tmp:
            # KiCad finds a board's project by its file name, so the routed board is judged as a copy named like the
            # input, beside copies of the input's .kicad_pro / .kicad_dru: both boards under the same rules.
            staged = []
            for tag, board in (("input", args.input), ("routed", args.routed)):
                folder = Path(tmp) / tag
                folder.mkdir()
                shutil.copyfile(board, folder / args.input.name)
                for ext in (".kicad_pro", ".kicad_dru"):
                    if args.input.with_suffix(ext).is_file():
                        shutil.copyfile(args.input.with_suffix(ext), folder / (args.input.stem + ext))
                staged.append(folder / args.input.name)
            try:
                before = drc(cli, staged[0], Path(tmp) / "input.json")
                after = drc(cli, staged[1], Path(tmp) / "routed.json")
            except RuntimeError as exc:
                if args.stage_dir:
                    raise
                # The copies exist, so a kicad-cli that cannot open them does not see this directory.
                raise RuntimeError(f"{exc}\nThe boards were staged in {tmp}. A kicad-cli that runs in a container or sandbox "
                                   "(Docker, Flatpak, Snap) may not see that directory: pass --stage-dir DIR with one it can "
                                   "read, for example a build directory of the project.") from exc
        old = Counter(v["type"] for v in before["violations"] if routing_error(v))
        new = Counter(v["type"] for v in after["violations"] if routing_error(v))
        added = {k: new[k] - old[k] for k in sorted(new) if new[k] > old[k]}
        examples = {k: [{"description": flat(v.get("description", "")),
                         "items": [flat(i.get("description", "")) for i in v.get("items", [])]}
                        for v in after["violations"] if v["type"] == k and routing_error(v)][:5]
                    for k in added}
        unconnected = len(after["unconnected_items"])
        verdict = "ILLEGAL" if added else "INCOMPLETE" if unconnected else "CLEAN"
        result.update(unconnected_before=len(before["unconnected_items"]), unconnected_after=unconnected,
                      errors_before=dict(sorted(old.items())), errors_after=dict(sorted(new.items())),
                      added_errors=added, examples=examples, verdict=verdict)
        if args.route_json:
            route = json.loads(args.route_json.read_text())
            result["router"] = {k: route.get(k) for k in ("routed", "connections", "tracks", "vias", "expansions", "variant_name")}
            result["router"]["failures"] = len(route.get("failures") or [])
        result["refill_reminder"] = ("Refill the zones after promoting the routed board to the project's board file (KiCad: B, then save); "
                                     "this judge only refills in memory, and a refill needs the board's own project.")
        if save_board:
            result["refill_command"] = shlex.join([cli, "pcb", "drc", "--refill-zones", "--save-board", str(args.input)])
        result["staging_note"] = f"Both boards were judged with {args.input.stem}.kicad_pro / .kicad_dru."
        code = {"CLEAN": 0, "INCOMPLETE": 1, "ILLEGAL": 2}[verdict]
        print(f"# TraceMaker sign-off: {verdict}\n")
        for warning in result["warnings"]:
            print(f"- WARNING: {warning}")
        print(f"- Unconnected: {result['unconnected_before']} → {unconnected}")
        print(f"- Added routing errors: {sum(added.values())} (positive per-type output − input deltas)")
        for kind, count in added.items():
            print(f"\n## {flat(kind)}: +{count}")
            print("Examples from the output report; deltas do not identify individual new violations.")
            for example in examples[kind]:
                print(f"- {example['description']}: " + "; ".join(example["items"]))
        if "router" in result:
            print("\n## Router summary")
            for key, value in result["router"].items():
                print(f"- {key}: {flat(value) if value is not None else 'not recorded'}")
        print(f"\n{result['refill_reminder']}")
        if save_board:
            print(f"After promoting, from the command line: `{result['refill_command']}`")
        print(result["staging_note"])
    except (OSError, ValueError, RuntimeError, subprocess.TimeoutExpired) as exc:
        result.update(verdict="TOOL ERROR", error=str(exc))
        print(f"# TraceMaker sign-off: TOOL ERROR\n\n{exc}", file=sys.stderr)
        code = 3
    if args.json:
        try:
            args.json.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        except OSError as exc:
            print(f"Cannot write JSON report: {exc}", file=sys.stderr)
            return 3
    return code


if __name__ == "__main__":
    sys.exit(main())
