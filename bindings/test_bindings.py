# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks the Python module `tracemaker` against the CLI (no pytest needed).

usage: test_bindings.py MODULE_DIR [TRACEMAKER_BINARY]
Exit code 77 (ctest: skipped) when the module was not built or the fixture board is missing.
"""
import json
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
BOARD = os.path.join(ROOT, "bench/data/freerouting/scripts/benchmark/fixtures/PCBench/C-BISCUIT_buck-reg-5v/unrouted.kicad_pcb")
# Deterministic run: a work budget instead of wall time, no knowledge base, CPU fields (the GPU may be busy).
ROUTE = dict(work=2_000_000, threads=8, seed=1, kb=False, gpu=False)
CLI = ["--work", "2000000", "--threads", "8", "--seed", "1", "--no-kb", "--no-gpu"]


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    sys.path.insert(0, sys.argv[1])
    try:
        import tracemaker
    except ImportError as e:
        print(f"SKIP: tracemaker module not importable from {sys.argv[1]}: {e}")
        return 77
    if not os.path.exists(BOARD):
        print(f"SKIP: fixture missing: {BOARD} (scripts/fetch_fixtures.sh freerouting)")
        return 77
    exe = sys.argv[2] if len(sys.argv) > 2 else None

    b = tracemaker.read_board(BOARD)
    print(b)
    assert b.copper_layers == ["F.Cu", "B.Cu"], b.copper_layers
    assert b.nets > 0 and b.footprints > 0 and b.pads > 0 and b.tracks == 0 and b.vias == 0
    assert b.outline_mm is not None and b.outline_mm[0] > 0 and b.outline_mm[1] > 0
    assert len(b.net_names) == b.nets

    with tempfile.TemporaryDirectory(prefix="tm-bindings-") as tmp:
        out = os.path.join(tmp, "py.kicad_pcb")
        items_path = os.path.join(tmp, "py_items.json")
        r = tracemaker.route(BOARD, out, items_out=items_path, **ROUTE)
        print({k: r[k] for k in ("routed", "connections", "tracks", "vias", "seconds", "exit_code")})
        assert r["connections"] > 0 and r["routed"] == r["connections"], r
        assert r["exit_code"] == 0 and r["unrouted"] == [] and r["failures"] == []
        assert r["tracks"] > 0 and os.path.exists(out)
        assert any(line.startswith("routed ") for line in r["log"]), r["log"]

        items = tracemaker.emit_items(BOARD, **ROUTE)
        assert len(items["tracks"]) == r["tracks"] and len(items["vias"]) == r["vias"]
        with open(items_path) as f:
            assert json.load(f) == items, "emit_items() differs from route(items_out=...)"

        violations = tracemaker.drc(out)
        errors = [v for v in violations if v["severity"] == "error"]
        print(f"drc: {len(violations)} items, {len(errors)} errors")
        assert not any(v["type"] == "unconnected_items" for v in violations), "routed board still has unconnected items"
        for v in violations:
            assert {"type", "severity", "description", "items", "actual_mm", "required_mm", "layer"} <= v.keys()

        if exe:  # the module and the CLI share run_route_job: identical boards and items
            cli_out = os.path.join(tmp, "cli.kicad_pcb")
            cli_items = os.path.join(tmp, "cli_items.json")
            p = subprocess.run([exe, "route", BOARD, "-o", cli_out, "--emit-items", cli_items] + CLI, capture_output=True, text=True)
            assert p.returncode == 0, p.stdout + p.stderr
            with open(cli_out, "rb") as f1, open(out, "rb") as f2:
                assert f1.read() == f2.read(), "CLI and bindings wrote different boards"
            with open(cli_items) as f:
                assert json.load(f) == items, "CLI and bindings emitted different items"
            cli_drc = os.path.join(tmp, "cli_drc.json")
            subprocess.run([exe, "drc", out, "--json", cli_drc], capture_output=True, text=True)
            with open(cli_drc) as f:
                d = json.load(f)
            assert len(d["violations"]) + len(d["unconnected_items"]) == len(violations), "drc() differs from `tracemaker drc`"
            print("CLI parity: identical board, items and DRC count")

    print("OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
