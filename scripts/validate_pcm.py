#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Checks a KiCad PCM package archive (e.g. build/pcm/tracemaker-0.1.0.zip).

Always runs the standard-library checks below. When kicad-python's validator is importable (kipy.packaging,
kicad-python >= 0.8, with jsonschema), it also runs that: the official PCM v1/v2 and IPC plugin JSON schemas.

usage: validate_pcm.py ARCHIVE [--require-kipy]
Exit code 0 = valid, 1 = errors found.
"""
from __future__ import annotations

import argparse
import json
import re
import struct
import sys
import zipfile
from pathlib import PurePosixPath

REQUIRED = {"name": str, "description": str, "description_full": str, "identifier": str, "type": str, "author": dict,
            "license": str, "resources": dict, "versions": list}
TYPES = {"plugin", "library", "colortheme", "fab"}
STATUS = {"stable", "testing", "development", "deprecated"}
ID_RE = re.compile(r"^[a-zA-Z][-a-zA-Z0-9.]{0,98}[a-zA-Z0-9]$")
VERSION_RE = re.compile(r"^\d{1,4}(\.\d{1,4}(\.\d{1,6})?)?$")
KICAD_RE = re.compile(r"^\d{1,2}(\.\d{1,2}(\.\d{1,2})?)?$")
TOP_LEVEL = {"metadata.json", "plugins", "resources", "symbols", "footprints", "3dmodels", "colors", "templates", "scripting"}


def png_size(data: bytes) -> tuple[int, int] | None:
    if data[:8] != b"\x89PNG\r\n\x1a\n" or data[12:16] != b"IHDR":
        return None
    return struct.unpack(">II", data[16:24])


def check(path: str) -> list[str]:
    errors: list[str] = []
    try:
        z = zipfile.ZipFile(path)
    except (OSError, zipfile.BadZipFile) as e:
        return [f"not a zip archive: {e}"]
    with z:
        names = z.namelist()
        files = {n for n in names if not n.endswith("/")}
        for n in names:
            p = PurePosixPath(n)
            if p.is_absolute() or ".." in p.parts or "\\" in n:
                errors.append(f"unsafe path in archive: {n}")
            elif p.parts[0] not in TOP_LEVEL:
                errors.append(f"unexpected top-level entry: {n}")
        if z.testzip() is not None:
            errors.append("archive has a corrupt member")
        if "metadata.json" not in files:
            return errors + ["metadata.json missing from the archive root"]
        try:
            meta = json.loads(z.read("metadata.json"))
        except json.JSONDecodeError as e:
            return errors + [f"metadata.json is not valid JSON: {e}"]

        if "https://go.kicad.org/pcm/schemas/v" not in str(meta.get("$schema", "")):
            errors.append("metadata.json: $schema must reference https://go.kicad.org/pcm/schemas/v1 or v2")
        for key, typ in REQUIRED.items():
            if key not in meta:
                errors.append(f"metadata.json: missing required key '{key}'")
            elif not isinstance(meta[key], typ):
                errors.append(f"metadata.json: '{key}' must be a {typ.__name__}")
        if len(meta.get("name", "")) > 200 or len(meta.get("description", "")) > 500 or len(meta.get("description_full", "")) > 5000:
            errors.append("metadata.json: name/description/description_full too long (200/500/5000)")
        if isinstance(meta.get("identifier"), str) and not ID_RE.match(meta["identifier"]):
            errors.append(f"metadata.json: invalid identifier '{meta['identifier']}'")
        if meta.get("type") not in TYPES:
            errors.append(f"metadata.json: type must be one of {sorted(TYPES)}")
        author = meta.get("author")
        if isinstance(author, dict) and not (isinstance(author.get("name"), str) and isinstance(author.get("contact"), dict)):
            errors.append("metadata.json: author needs 'name' (string) and 'contact' (object)")
        versions = meta.get("versions") or []
        if not versions:
            errors.append("metadata.json: 'versions' must list at least one version")
        for i, v in enumerate(versions):
            where = f"metadata.json: versions[{i}]"
            if not isinstance(v, dict):
                errors.append(f"{where} must be an object")
                continue
            for key in ("version", "status", "kicad_version"):
                if key not in v:
                    errors.append(f"{where}: missing required key '{key}'")
            if "version" in v and not VERSION_RE.match(str(v["version"])):
                errors.append(f"{where}: invalid version '{v['version']}'")
            if "status" in v and v["status"] not in STATUS:
                errors.append(f"{where}: status must be one of {sorted(STATUS)}")
            for key in ("kicad_version", "kicad_version_max"):
                if key in v and not KICAD_RE.match(str(v[key])):
                    errors.append(f"{where}: invalid {key} '{v[key]}'")
            if any(k.startswith("download_") for k in v):
                errors.append(f"{where}: download_* keys belong in the repository, not in the archive")
            if v.get("runtime", "swig") not in ("swig", "ipc"):
                errors.append(f"{where}: runtime must be 'swig' or 'ipc'")

        if meta.get("type") == "plugin":
            ipc = any(isinstance(v, dict) and v.get("runtime") == "ipc" for v in versions)
            if ipc:
                if "plugins/plugin.json" not in files:
                    errors.append("IPC plugin: plugins/plugin.json missing")
                else:
                    try:
                        plugin = json.loads(z.read("plugins/plugin.json"))
                    except json.JSONDecodeError as e:
                        plugin = {}
                        errors.append(f"plugins/plugin.json is not valid JSON: {e}")
                    for key in ("identifier", "name", "description", "runtime", "actions"):
                        if key not in plugin:
                            errors.append(f"plugins/plugin.json: missing '{key}'")
                    for a in plugin.get("actions", []):
                        ep = a.get("entrypoint", "")
                        if f"plugins/{ep}" not in files:
                            errors.append(f"plugins/plugin.json: entrypoint '{ep}' not in the archive")
                    if plugin.get("runtime", {}).get("type") == "python" and "plugins/requirements.txt" not in files:
                        errors.append("Python IPC plugin: plugins/requirements.txt missing (kicad-python)")
            elif "plugins/__init__.py" not in files:
                errors.append("SWIG plugin: plugins/__init__.py missing")

        if "resources/icon.png" not in files:
            errors.append("resources/icon.png missing")
        else:
            size = png_size(z.read("resources/icon.png"))
            if size is None:
                errors.append("resources/icon.png is not a PNG")
            elif size != (64, 64):
                errors.append(f"resources/icon.png is {size[0]}x{size[1]}, KiCad expects 64x64")
    return errors


def kipy_check(path: str) -> list[str] | None:
    """kicad-python's official validator; None when it is not installed."""
    try:
        from kipy.packaging.validate import validate
    except ImportError:
        return None
    report = validate(path)
    out = []
    for m in report.messages:
        if m.level == "error":
            out.append(f"kipy: {m.message}" + (f" ({m.path})" if m.path else ""))
        elif m.level == "warning":
            print(f"kipy warning: {m.message}" + (f" ({m.path})" if m.path else ""))
    return out


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("archive")
    ap.add_argument("--require-kipy", action="store_true", help="fail if kicad-python's validator is not available")
    args = ap.parse_args()
    errors = check(args.archive)
    k = kipy_check(args.archive)
    if k is None:
        msg = "kicad-python validator not available (pip install 'kicad-python>=0.8'); standard-library checks only"
        if args.require_kipy:
            errors.append(msg)
        else:
            print(msg)
    else:
        errors += k
        print("kicad-python validator: ran")
    for e in errors:
        print(f"ERROR: {e}")
    if errors:
        return 1
    print(f"OK: {args.archive}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
