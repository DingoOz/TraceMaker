#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert a component-rule override file from YAML to the JSON the engine reads (design doc 15 §6.3).

  scripts/crules_override.py board.tracemaker_rules.yaml -o board.tracemaker_rules.json
  tracemaker rules board.kicad_pcb --rules-override board.tracemaker_rules.json

The YAML schema maps 1:1 to JSON:

  disable: [USB2-04@J2, XTAL-04, mounting_hole]     # rule ids or categories, optionally @REF
  assert: {J5: usb2}                                 # force a category on a part
  deny: {U7: buck}                                   # forbid a category on a part
  set:                                               # parameter overrides
    - {rule: XTAL-02, param: max_mm, value: 5}
    - {rule: XTAL-01@Y1, param: max_mm, value: 8}

This script only converts (keys and order are kept); the engine validates everything against the catalogue and
the board and refuses unknown keys, rule ids, categories, parameters and references.
"""
import argparse
import json
import pathlib
import sys


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("yaml_file")
    ap.add_argument("-o", "--output", help="JSON file to write (default: stdout)")
    a = ap.parse_args()
    try:
        import yaml  # noqa: PLC0415 (optional dependency: only this script needs it)
    except ImportError:
        print("PyYAML is not installed (pip install pyyaml); write the override file as JSON instead", file=sys.stderr)
        return 2
    data = yaml.safe_load(pathlib.Path(a.yaml_file).read_text())
    if data is None:
        data = {}
    if not isinstance(data, dict):
        print(f"{a.yaml_file}: the top level must be a map", file=sys.stderr)
        return 1
    text = json.dumps(data, indent=1, ensure_ascii=False) + "\n"
    if a.output:
        pathlib.Path(a.output).write_text(text)
    else:
        sys.stdout.write(text)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
