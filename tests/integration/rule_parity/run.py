#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Compare generated D80 boards with frozen KiCad violation multisets, without routing.

  run.py <tracemaker> <work-dir>
  run.py <tracemaker> <work-dir> --rejudge [--kicad-cli kicad-cli]

Only --rejudge needs KiCad (exit 77 when its executable is absent). Reports and
logs stay in work-dir; a failed rejudge never replaces the committed oracle.
"""
import argparse
import collections
import json
import math
import pathlib
import re
import subprocess
import tempfile

from generate import generate


def summarize(report: dict, meta: dict, kicad: bool = False) -> collections.Counter:
    """Count (violation type, sorted item labels), retaining duplicate violations.

    KiCad identifies items by UUID. TraceMaker's JSON identifies them by kind,
    position (mm), and track layer. Ambiguous or unknown identities are errors,
    not labels that can accidentally make an incomplete oracle pass.
    """
    if report.get("coordinate_units") != "mm":
        raise ValueError("report coordinate_units must be mm")
    violations = report.get("violations")
    if not isinstance(violations, list):
        raise ValueError("report has no violations array")
    selected = set(meta["types"])
    counts = collections.Counter()
    for violation in violations:
        if not isinstance(violation, dict) or not isinstance(violation.get("type"), str):
            raise ValueError("malformed violation entry")
        violation_type = violation["type"]
        if violation_type not in selected:
            continue
        items = violation.get("items")
        if not isinstance(items, list) or not items:
            raise ValueError(f"{violation_type}: missing violation items")
        labels = []
        for item in items:
            if not isinstance(item, dict):
                raise ValueError(f"{violation_type}: malformed item {item!r}")
            if kicad:
                label = meta["ids"].get(item.get("uuid"))
                if label is None:
                    raise ValueError(f"{violation_type}: unmapped KiCad UUID {item.get('uuid')!r}")
            else:
                description = item.get("description", "")
                position = item.get("pos", {})
                if not isinstance(description, str) or not isinstance(position, dict):
                    raise ValueError(f"{violation_type}: malformed item {item!r}")
                kind = description.split(maxsplit=1)[0] if description else ""
                layer = re.search(r"\bon ([\w.]+)", description)
                candidates = []
                for candidate, spec in meta["items"].items():
                    if spec["kind"] != kind:
                        continue
                    if "layer" in spec and layer and spec["layer"] != layer.group(1):
                        continue
                    # The JSON mm representation can round fixed-point nm positions.
                    if all(isinstance(position.get(axis), (int, float)) and
                           math.isclose(position[axis], value, rel_tol=0, abs_tol=0.000001)
                           for axis, value in zip(("x", "y"), spec["pos"])):
                        candidates.append(candidate)
                if len(candidates) != 1:
                    raise ValueError(f"{violation_type}: unmapped/ambiguous TraceMaker item "
                                     f"{item!r}; candidate labels: {sorted(candidates)}")
                label = candidates[0]
            labels.append(label)
        counts[(violation_type, tuple(sorted(labels)))] += 1
    return counts


def frozen_case(counts: collections.Counter) -> dict:
    entries = {}
    for (violation_type, labels), count in sorted(counts.items()):
        entries.setdefault(violation_type, []).extend([list(labels) for _ in range(count)])
    return entries


def expected_counts(entries: dict, meta: dict) -> collections.Counter:
    if not isinstance(entries, dict):
        raise ValueError("expected case must be an object")
    counts = collections.Counter()
    for violation_type, pairs in entries.items():
        if violation_type not in meta["types"]:
            raise ValueError(f"unexpected oracle violation type {violation_type!r}")
        if not isinstance(pairs, list):
            raise ValueError(f"{violation_type}: expected item lists array")
        for labels in pairs:
            if not isinstance(labels, list) or not labels or any(
                    not isinstance(label, str) or label not in meta["ids"].values() for label in labels):
                raise ValueError(f"{violation_type}: invalid oracle labels {labels!r}")
            counts[(violation_type, tuple(sorted(labels)))] += 1
    return counts


def print_difference(name: str, expected: collections.Counter, actual: collections.Counter) -> bool:
    missing, unexpected = expected - actual, actual - expected
    if not missing and not unexpected:
        return False
    print(f"FAIL {name}: violation multiset differs")
    for title, counts in (("missing", missing), ("unexpected", unexpected)):
        for (violation_type, labels), count in sorted(counts.items()):
            print(f"  {title}: {count} x {violation_type} [{', '.join(labels)}]")
    return True


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("tracemaker")
    parser.add_argument("work_dir", type=pathlib.Path)
    parser.add_argument("--rejudge", action="store_true")
    parser.add_argument("--expected", type=pathlib.Path,
                        default=pathlib.Path(__file__).with_name("expected.json"))
    parser.add_argument("--kicad-cli", default="kicad-cli")
    args = parser.parse_args()
    work = args.work_dir.resolve()
    try:
        metadata = generate(work)
        if not metadata:
            raise ValueError("generator produced no cases")
        expected = None
        if not args.rejudge:
            expected = json.loads(args.expected.read_text())
            if (not isinstance(expected, dict) or expected.get("schema_version") != 1 or
                    not isinstance(expected.get("cases"), dict) or
                    not isinstance(expected.get("judge"), str)):
                raise ValueError("expected.json must have schema_version 1, a judge string and a cases object")
    except (OSError, ValueError, KeyError, TypeError) as error:
        print(f"FAIL: preparing parity corpus: {error}")
        return 1

    failed = 0
    cases = {}
    versions = set()
    if expected is not None:
        for name in sorted(set(expected["cases"]) - set(metadata)):
            print(f"FAIL {name}: oracle case has no generated board")
            failed += 1
    for name, meta in sorted(metadata.items()):
        directory = work / name
        board = directory / "b.kicad_pcb"
        engine = "kicad" if args.rejudge else "tracemaker"
        report_path = directory / f"{engine}.json"
        log_path = directory / f"{engine}.log"
        command = ([args.kicad_cli, "pcb", "drc", "--format", "json", "--severity-all",
                    "--all-track-errors", "--exit-code-violations", "-o", str(report_path), str(board)]
                   if args.rejudge else
                   [args.tracemaker, "drc", str(board), "--json", str(report_path)])
        try:
            report_path.unlink(missing_ok=True)
            result = subprocess.run(command, capture_output=True, text=True)
            log_path.write_text(f"command: {command!r}\nexit: {result.returncode}\n"
                                f"stdout:\n{result.stdout}\nstderr:\n{result.stderr}")
            if result.returncode not in (0, 5):
                raise ValueError(f"DRC exited {result.returncode}; see {log_path}")
            report = json.loads(report_path.read_text())
            actual = summarize(report, meta, kicad=args.rejudge)
            if args.rejudge:
                version = report.get("kicad_version")
                if not isinstance(version, str) or not version.strip():
                    raise ValueError("KiCad report has no kicad_version provenance")
                versions.add(version)
                cases[name] = frozen_case(actual)
            elif name not in expected["cases"]:
                raise ValueError("generated case is missing from frozen oracle")
            else:
                failed += print_difference(name, expected_counts(expected["cases"][name], meta), actual)
        except FileNotFoundError as error:
            if error.filename == command[0] and args.rejudge:
                log_path.write_text(f"command: {command!r}\nerror: {error}\n")
                print(f"SKIP: KiCad executable absent: {args.kicad_cli}")
                return 77
            print(f"FAIL {name}: {error}")
            failed += 1
            if error.filename == command[0]:
                log_path.write_text(f"command: {command!r}\nerror: {error}\n")
                return 1
        except (OSError, ValueError, KeyError, TypeError, AttributeError) as error:
            print(f"FAIL {name}: {error}; reports/logs: {directory}")
            failed += 1
    if args.rejudge and not failed:
        if len(versions) != 1:
            print(f"FAIL: inconsistent KiCad versions: {sorted(versions)}")
            return 1
        oracle = {"schema_version": 1, "judge": f"KiCad {next(iter(versions))}", "cases": cases}
        try:
            # Same-directory rename makes replacement atomic; no partial oracle on failure.
            with tempfile.NamedTemporaryFile(mode="w", dir=args.expected.parent,
                                             prefix=args.expected.name + ".", delete=False) as output:
                temporary = pathlib.Path(output.name)
                output.write(json.dumps(oracle, indent=2, sort_keys=True) + "\n")
            temporary.replace(args.expected)
        except OSError as error:
            if "temporary" in locals():
                temporary.unlink(missing_ok=True)
            print(f"FAIL: writing frozen oracle: {error}")
            return 1
        print(f"Rejudged {len(cases)} cases with {oracle['judge']}: {args.expected}")
    elif not failed:
        print(f"PASS: {len(metadata)} cases match {expected['judge']}; reports/logs: {work}")
    else:
        print(f"FAIL: {failed} case/error differences; reports/logs: {work}")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
