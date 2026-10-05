#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Convert the component-rule catalogue (docs/component_rules.yaml) to the JSON the engine embeds.

  scripts/crules_catalogue.py            write src/crules/catalogue.json from the YAML
  scripts/crules_catalogue.py --check    exit 1 if the JSON does not match the YAML, or if the rule ids in the
                                         YAML and in docs/15-component-rules.md differ (doc 15 §9.1 L0)

The engine has no YAML parser (and should not grow one for a single file), so the YAML stays the file people
edit and the JSON is generated from it and committed. The check runs in ctest so the two cannot drift.
Exit code 77 (ctest "skipped") when PyYAML is not installed.
"""
import argparse
import json
import pathlib
import re
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
YAML = ROOT / "docs" / "component_rules.yaml"
JSON = ROOT / "src" / "crules" / "catalogue.json"
DOC = ROOT / "docs" / "15-component-rules.md"


def load_yaml() -> dict:
    try:
        import yaml  # noqa: PLC0415 (optional dependency: only this script needs it)
    except ImportError:
        print("PyYAML not installed: skipped", file=sys.stderr)
        raise SystemExit(77)
    return yaml.safe_load(YAML.read_text())


def render(cat: dict) -> str:
    # Catalogue order is meaningful (doc 15 §2.4: categories are processed in catalogue order), so keys and lists
    # keep the YAML order; indent=1 keeps diffs readable. ASCII only (JSON escapes) so the build can embed it in
    # string-literal chunks split at any byte.
    return json.dumps(cat, indent=1, ensure_ascii=True) + "\n"


def doc_rule_ids(prefixes: set) -> set:
    text = DOC.read_text()
    ids = set()
    for m in re.finditer(r"\b([A-Z][A-Z0-9]*)-(\d{2})\b", text):
        if m.group(1) in prefixes:
            ids.add(m.group(0))
    return ids


def check(cat: dict) -> int:
    errors, warnings = [], []
    if not JSON.exists():
        errors.append(f"{JSON} missing: run scripts/crules_catalogue.py")
    elif json.loads(JSON.read_text()) != cat:
        errors.append(f"{JSON.relative_to(ROOT)} differs from {YAML.relative_to(ROOT)}: run scripts/crules_catalogue.py")
    yaml_ids = [r["id"] for c in cat["categories"] for r in c["rules"]]
    if len(yaml_ids) != len(set(yaml_ids)):
        errors.append("duplicate rule ids in the YAML")
    prefixes = {c["prefix"] for c in cat["categories"]}
    doc_ids = doc_rule_ids(prefixes)
    for rid in sorted(set(yaml_ids) - doc_ids):
        errors.append(f"rule {rid} is in the YAML but not in {DOC.name}")
    for rid in sorted(doc_ids - set(yaml_ids)):
        errors.append(f"rule {rid} is in {DOC.name} but not in the YAML")
    for c in cat["categories"]:
        for r in c["rules"]:
            if not r["id"].startswith(c["prefix"] + "-"):
                errors.append(f"rule {r['id']} does not carry its category prefix {c['prefix']}")
            for key in ("kind", "severity", "sources", "enforce"):
                if not r.get(key):
                    errors.append(f"rule {r['id']} has no {key}")
            for s in r.get("sources", []):
                if s not in cat["sources"]:
                    errors.append(f"rule {r['id']} cites unknown source {s}")
            if r.get("severity") not in ("hard", "soft", "advisory"):
                errors.append(f"rule {r['id']}: bad severity {r.get('severity')}")
            if r.get("evidence") not in ("V", "R", "D", "C"):
                errors.append(f"rule {r['id']}: bad evidence {r.get('evidence')!r} (V, D, R or C, doc 15 §8)")
            if r.get("evidence") in ("R", "D") and r.get("severity") == "hard":
                warnings.append(f"rule {r['id']}: {r['evidence']}-evidence rule is hard; the engine demotes it to soft (doc 15 §8)")
        for d in c["detect"]:
            if not isinstance(d.get("weight"), int) or not 0 <= d["weight"] <= 100:
                errors.append(f"category {c['id']}: detector weight must be an integer 0..100")
            if d["signal"] != "topology":
                try:
                    re.compile(d["pattern"], re.I)
                except re.error as e:
                    errors.append(f"category {c['id']}: bad pattern {d['pattern']!r}: {e}")
    for w in warnings:
        print("warning:", w, file=sys.stderr)
    for e in errors:
        print("error:", e, file=sys.stderr)
    if not errors:
        print(f"catalogue ok: {len(cat['categories'])} categories, {len(yaml_ids)} rules, JSON and doc in sync")
    return 1 if errors else 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="verify instead of writing")
    a = ap.parse_args()
    cat = load_yaml()
    if a.check:
        return check(cat)
    JSON.parent.mkdir(parents=True, exist_ok=True)
    JSON.write_text(render(cat))
    print(f"wrote {JSON.relative_to(ROOT)}")
    return check(cat)


if __name__ == "__main__":
    raise SystemExit(main())
