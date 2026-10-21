# SPDX-License-Identifier: GPL-3.0-or-later
"""Stdlib-only physical-instance gates for KiCad DRC JSON (doc 10, D106)."""
from collections import Counter
from decimal import Decimal, ROUND_HALF_EVEN
import re

JUDGE_CODE_VERSION = "instance-v3-graphic-arcs"
JUDGE_FLAGS = ["--format", "json", "--severity-all", "--all-track-errors"]
# KiCad 10.0.x stops reporting at these counts, across severities. A report at
# the boundary is potentially truncated, not proof that exactly this many exist.
CAPS = {"clearance": 499, "unconnected_items": 499, "default": 199}
UNIT_NM = {"mm": 1000000, "in": 25400000, "inch": 25400000,
           "mils": 25400, "mil": 25400, "nm": 1}
NOT_ROUTING = frozenset({
    "lib_footprint_issues", "lib_footprint_mismatch", "silk_overlap", "silk_over_copper", "silk_edge_clearance",
    "text_height", "text_thickness", "courtyards_overlap", "missing_courtyard", "malformed_courtyard",
    "footprint_type_mismatch", "footprint_filters_mismatch", "nonmirrored_text_on_back_layer",
    "npth_inside_courtyard", "pth_inside_courtyard", "duplicate_footprints", "extra_footprint",
    "missing_footprint", "footprint_symbol_mismatch", "unconnected_items", "track_dangling", "via_dangling",
    "holes_co_located",
})
REFILL_ROUTING_TYPES = frozenset({"starved_thermal", "isolated_copper"})
PLACEMENT_ERRORS = frozenset({
    "courtyards_overlap", "pth_inside_courtyard", "npth_inside_courtyard", "copper_edge_clearance", "clearance",
    "shorting_items", "solder_mask_bridge", "hole_clearance", "hole_to_hole", "malformed_courtyard",
}) | REFILL_ROUTING_TYPES


def _nm(value, unit):
    number = Decimal(str(value)) * UNIT_NM[unit]
    if not number.is_finite():
        raise ValueError("non-finite DRC coordinate")
    return int(number.to_integral_value(rounding=ROUND_HALF_EVEN))


def _description(text):
    # Footprint references and UUIDs can be regenerated without changing copper.
    text = re.sub(r"\s+of\s+\S+", "", text)
    return re.sub(r"(-?\d+(?:\.\d+)?)\s*(mm|inch|in|mils|mil|nm)\b",
                  lambda m: f"{_nm(m[1], m[2])}nm", text)


def _routed_item(description):
    # An arc without a net is a copper graphic of the board or of a footprint, not a routed arc track. KiCad names a different piece of the same
    # graphic from run to run (PCBench MySensors_RNDKeyfob, livolo), so counting it as routed made a pre-existing defect look added.
    if description.startswith(("Track", "Via")):
        return True
    return description.startswith("Arc") and "[<no net>]" not in description


def routed_copper(violation):
    return any(_routed_item(i.get("description", "")) for i in violation.get("items", []))


def own_footprint(violation):
    # A rigid footprint cannot change its internal pad/graphic relationships.
    refs = [re.search(r" of ([^ ]+)(?: on [^ ]+)?$", i.get("description", ""))
            for i in violation.get("items", [])]
    return bool(refs) and all(refs) and len({m[1] for m in refs}) == 1


def routing_error(violation):
    return violation["type"] not in NOT_ROUTING and (
        violation["type"] in REFILL_ROUTING_TYPES or routed_copper(violation))


def placement_error(violation):
    # A pad-only thermal finding depends on the surrounding zone, not only on
    # the rigid footprint; it is not an immutable single-footprint defect.
    return violation["type"] in PLACEMENT_ERRORS and (
        violation["type"] in REFILL_ROUTING_TYPES or not own_footprint(violation))


def violations(report, *, predicate=None):
    """Error multiset: type, physical items, severity; item order is irrelevant."""
    unit = report.get("coordinate_units", "mm")
    if unit not in UNIT_NM:
        raise ValueError(f"unsupported DRC coordinate_units: {unit!r}")
    instances = Counter()
    for violation in report.get("violations", []):
        if violation.get("severity") != "error" or (predicate and not predicate(violation)):
            continue
        items = []
        for item in violation.get("items", []):
            pos = item.get("pos", {})
            position = tuple(_nm(pos[axis], unit) for axis in ("x", "y")) if pos else ()
            items.append(("" if position else item.get("uuid", ""),
                          _description(item.get("description", "")), position))
        # PTH pads and multilayer zones do not name the affected copper layer
        # in their item labels. Keep the violation's layer context, but not
        # variable measurements such as minimum/actual thermal spoke counts.
        layers = tuple(sorted(set(re.findall(r"\blayer\s+([\w.]+)", violation.get("description", "")))))
        signature = (violation["type"], "error", tuple(sorted(items)),
                     _description(violation.get("description", "")) if not items else "", layers)
        instances[signature] += 1
    return instances


def instance_counts(instances):
    counts = Counter()
    for signature, number in instances.items():
        counts[signature[0]] += number
    return dict(sorted(counts.items()))


def summarize(report):
    """Retain native instances alongside the historical drc() count fields."""
    return {**report, "errors": instance_counts(violations(report)),
            "routed_errors": instance_counts(violations(report, predicate=routing_error)),
            "cross_errors": instance_counts(violations(report, predicate=lambda v: not own_footprint(v))),
            "unconnected": len(report.get("unconnected_items", []))}


def capped_types(report):
    counts = Counter(v["type"] for v in report.get("violations", []))
    return {kind: count for kind, count in sorted(counts.items())
            if count >= CAPS.get(kind, CAPS["default"])}


def added_errors(before, after, *, placement=False):
    """A removed error cannot cancel a new physical instance of the same type.

    Capped types with errors on either side are unknown even if the listed
    errors are diagnostic-only: the omitted instances may involve routed copper.
    Warning-only saturation and immutable routing types remain diagnostic.
    """
    capped = {"before": capped_types(before), "after": capped_types(after)}
    excluded = set(capped["before"]) | set(capped["after"])
    error_types = {v["type"] for report in (before, after)
                   for v in report.get("violations", []) if v.get("severity") == "error"}
    evaluated = excluded & error_types
    evaluated = evaluated & PLACEMENT_ERRORS if placement else evaluated - NOT_ROUTING
    predicate = placement_error if placement else routing_error
    additions = violations(after, predicate=predicate) - violations(before, predicate=predicate)
    all_additions = violations(after) - violations(before)
    additions = Counter({key: n for key, n in additions.items() if key[0] not in excluded})
    all_additions = Counter({key: n for key, n in all_additions.items() if key[0] not in excluded})
    added = instance_counts(additions)
    return {"added_errors": added, "added_any": instance_counts(all_additions),
            "capped": capped, "capped_evaluated_types": sorted(evaluated),
            "capped_not_evaluated_types": sorted(excluded - evaluated),
            "error_gate": "unknown" if evaluated else "fail" if added else "pass"}


def clean(result, unconnected):
    return unconnected == 0 and result["error_gate"] == "pass"
