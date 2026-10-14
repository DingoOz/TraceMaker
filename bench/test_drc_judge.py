# SPDX-License-Identifier: GPL-3.0-or-later
import copy
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

import drc_judge as judge
import run as bench_run


def item(kind="Track", x=1, uuid="a", ref="R1", net="GND", layer="F.Cu"):
    return {"uuid": uuid, "description": f"{kind} [{net}] of {ref} on {layer}",
            "pos": {"x": x, "y": 2}}


def violation(typ="clearance", severity="error", items=None):
    return {"type": typ, "severity": severity, "items": items if items is not None else [item()]}


def report(*entries, unit="mm"):
    return {"coordinate_units": unit, "violations": list(entries), "unconnected_items": []}


class PhysicalGateTests(unittest.TestCase):
    def test_same_count_replacement_is_added(self):
        before, after = report(violation()), report(violation(items=[item(x=3)]))
        result = judge.added_errors(before, after)
        self.assertEqual(result["added_errors"], {"clearance": 1})
        self.assertFalse(judge.clean(result, 0))

    def test_multiplicity_and_item_order(self):
        a = violation(items=[item(), item("Pad", x=3)])
        b = violation(items=list(reversed(a["items"])))
        self.assertEqual(judge.added_errors(report(a), report(b))["error_gate"], "pass")
        self.assertEqual(judge.added_errors(report(a), report(b, b, b))["added_errors"], {"clearance": 2})

    def test_regenerated_ids_and_refs_are_not_physical_changes(self):
        before = report(violation())
        after = report(violation(items=[item(uuid="new", ref="REF123")]))
        self.assertEqual(judge.added_errors(before, after)["added_any"], {})

    def test_position_kind_net_and_layer_are_identity(self):
        for changed in (item(x=3), item("Via"), item(net="VCC"), item(layer="B.Cu")):
            with self.subTest(item=changed):
                self.assertEqual(judge.added_errors(report(violation()), report(violation(items=[changed])))["added_errors"], {"clearance": 1})

    def test_uuid_fallback_for_items_without_positions(self):
        a, b = item(uuid="a"), item(uuid="b")
        del a["pos"]
        del b["pos"]
        self.assertEqual(judge.added_errors(report(violation(items=[a])), report(violation(items=[b])))["added_errors"], {"clearance": 1})

    def test_coordinate_and_description_units_are_normalized(self):
        a = item(x=25.4)
        a["pos"]["y"] = 50.8
        a["description"] = "Track [GND] on F.Cu, length 25.4 mm"
        b = copy.deepcopy(a)
        b["pos"] = {"y": 2.0, "x": 1.0}
        b["description"] = "Track [GND] on F.Cu, length 1 in"
        self.assertEqual(judge.violations(report(violation(items=[a]))),
                         judge.violations(report(violation(items=[b]), unit="in")))
        with self.assertRaises(ValueError):
            judge.added_errors(report(), report(violation(), unit="unknown"))

    def test_severity_transition_is_not_cancelled(self):
        warning = report(violation("starved_thermal", "warning", [item("Pad")]))
        error = report(violation("starved_thermal", "error", [item("Pad")]))
        self.assertEqual(judge.added_errors(warning, error)["added_errors"], {"starved_thermal": 1})
        self.assertEqual(judge.added_errors(error, warning)["error_gate"], "pass")

    def test_refill_errors_gate_even_without_tracks(self):
        for typ in sorted(judge.REFILL_ROUTING_TYPES):
            for kind in ("Pad", "Zone", "Track", "Via", "Arc"):
                for severity in ("error", "warning"):
                    with self.subTest(typ=typ, kind=kind, severity=severity):
                        v = violation(typ, severity, [item(kind)])
                        result = judge.added_errors(report(), report(v))
                        self.assertEqual(result["error_gate"], "fail" if severity == "error" else "pass")
                        self.assertEqual(judge.added_errors(report(v), report(v))["error_gate"], "pass")
            a = violation(typ, items=[item("Pad"), item("Zone", x=0)])
            b = copy.deepcopy(a)
            b["items"][0]["pos"]["x"] = 3
            self.assertEqual(judge.added_errors(report(a), report(b))["added_errors"], {typ: 1})

    def test_thermal_same_items_different_layer_is_a_new_instance(self):
        pad = item("Pad", layer="*.Cu")
        pad["description"] = "PTH pad 1 [GND] of J1"
        zone = item("Zone", x=0, uuid="zone", layer="F.Cu and In2.Cu")
        a = violation("starved_thermal", items=[pad, zone])
        a["description"] = "Thermal relief connection to zone incomplete (layer F.Cu; zone min spoke count 2; actual 1)"
        b = copy.deepcopy(a)
        b["description"] = "Thermal relief connection to zone incomplete (layer In2.Cu; zone min spoke count 2; actual 1)"
        for placement in (False, True):
            with self.subTest(placement=placement):
                result = judge.added_errors(report(a), report(b), placement=placement)
                self.assertEqual(result["added_errors"], {"starved_thermal": 1})
                self.assertEqual(result["error_gate"], "fail")

    def test_thermal_same_layer_changed_spoke_details_is_not_new(self):
        pad = item("Pad", layer="*.Cu")
        pad["description"] = "PTH pad 1 [GND] of J1"
        zone = item("Zone", x=0, uuid="zone", layer="F.Cu and In2.Cu")
        a = violation("starved_thermal", items=[pad, zone])
        a["description"] = "Thermal relief connection to zone incomplete (layer F.Cu; zone min spoke count 2; actual 1)"
        b = copy.deepcopy(a)
        b["description"] = "Thermal relief connection to zone incomplete (layer F.Cu; zone min spoke count 3; actual 0)"
        for placement in (False, True):
            with self.subTest(placement=placement):
                result = judge.added_errors(report(a), report(b), placement=placement)
                self.assertEqual(result["added_errors"], {})
                self.assertEqual(result["error_gate"], "pass")

    def test_pad_noise_is_diagnostic_but_track_errors_gate(self):
        for typ in ("clearance", "solder_mask_bridge"):
            pad = judge.added_errors(report(), report(violation(typ, items=[item("Pad")])))
            self.assertEqual(pad["added_any"], {typ: 1})
            self.assertEqual(pad["error_gate"], "pass")
            self.assertEqual(judge.added_errors(report(), report(violation(typ)))["error_gate"], "fail")
        immutable = judge.added_errors(report(), report(violation("lib_footprint_mismatch")))
        self.assertEqual(immutable["error_gate"], "pass")

    def test_placement_keeps_rigid_footprint_policy(self):
        internal = violation(items=[item("Pad"), item("Graphic", x=4)])
        cross = violation(items=[item("Pad"), item("Pad", x=4, ref="R2")])
        self.assertEqual(bench_run.place_added(report(), report(internal)), {})
        self.assertEqual(bench_run.place_added(report(), report(cross)), {"clearance": 1})
        board_item = violation(items=[item("Pad"), {"description": "Zone [VCC] on F.Cu", "pos": {"x": 0, "y": 0}}])
        self.assertEqual(bench_run.place_added(report(), report(board_item)), {"clearance": 1})
        # A thermal depends on the plane even when KiCad names only its pad.
        for typ in judge.REFILL_ROUTING_TYPES:
            self.assertEqual(bench_run.place_added(report(), report(violation(typ, items=[item("Pad")]))), {typ: 1})

    def test_placement_same_count_replacement(self):
        before = report(violation(items=[item("Pad"), item("Pad", x=4, ref="R2")]))
        after = report(violation(items=[item("Pad"), item("Pad", x=5, ref="R2")]))
        self.assertEqual(bench_run.place_added(before, after), {"clearance": 1})

    def test_caps_boundary_and_both_sides(self):
        for typ in ("clearance", "starved_thermal", "isolated_copper", "track_width"):
            cap = judge.CAPS.get(typ, judge.CAPS["default"])
            below = report(*([violation(typ)] * (cap - 1)))
            self.assertEqual(judge.added_errors(below, below)["error_gate"], "pass")
            for size in (cap, cap + 1):
                capped = report(*([violation(typ)] * size))
                for before, after in ((capped, report()), (report(), capped), (capped, capped)):
                    with self.subTest(typ=typ, size=size):
                        result = judge.added_errors(before, after)
                        self.assertEqual(result["error_gate"], "unknown")
                        self.assertEqual(result["capped_evaluated_types"], [typ])
                        self.assertFalse(judge.clean(result, 0))

    def test_warning_only_caps_and_mixed_severities(self):
        warnings = report(*([violation("starved_thermal", "warning")] * 199))
        result = judge.added_errors(warnings, warnings)
        self.assertEqual(result["error_gate"], "pass")
        self.assertEqual(result["capped_not_evaluated_types"], ["starved_thermal"])
        for before, after in ((warnings, report(violation("starved_thermal"))),
                              (report(violation("starved_thermal")), warnings)):
            self.assertEqual(judge.added_errors(before, after)["error_gate"], "unknown")
        mixed = report(*([violation("clearance", "warning")] * 498), violation())
        self.assertEqual(judge.added_errors(report(), mixed)["error_gate"], "unknown")
        immutable = report(*([violation("lib_footprint_mismatch")] * 199))
        self.assertEqual(judge.added_errors(immutable, immutable)["error_gate"], "pass")

    def test_diagnostic_error_cap_is_conservative(self):
        pads = report(*([violation(items=[item("Pad")])] * 499))
        self.assertEqual(judge.added_errors(pads, pads)["error_gate"], "unknown")
        result = judge.added_errors(pads, pads, placement=True)
        self.assertEqual(result["error_gate"], "unknown")


class ComparisonCliTests(unittest.TestCase):
    def compare(self, result, *, place_gate=None):
        with tempfile.TemporaryDirectory() as directory:
            dirs = [Path(directory) / name for name in ("before", "after")]
            for path in dirs:
                path.mkdir()
                row = {"board": "synthetic", "completion": 1.0, "clean": judge.clean(result, 0), **result}
                if place_gate:
                    row["place_error_gate"] = place_gate
                (path / "boards.jsonl").write_text(json.dumps(row) + "\n")
            return subprocess.run([sys.executable, str(Path(__file__).with_name("compare_runs.py")), *map(str, dirs)],
                                  capture_output=True, text=True)

    def test_unknown_cannot_pass_even_when_both_runs_are_not_clean(self):
        cap = report(*([violation("track_width")] * 199))
        result = self.compare(judge.added_errors(cap, cap))
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("gate unknown", result.stdout)

    def test_warning_only_cap_comparison_passes(self):
        cap = report(*([violation("track_width", "warning")] * 199))
        result = self.compare(judge.added_errors(cap, cap))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_placement_unknown_cannot_pass(self):
        result = self.compare(judge.added_errors(report(), report()), place_gate="unknown")
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)


if __name__ == "__main__":
    unittest.main()
