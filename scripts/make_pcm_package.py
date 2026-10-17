#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the TraceMaker KiCad Plugin and Content Manager (PCM) package.

Output: build/pcm/tracemaker-<version>.zip, installable in KiCad 10 with
Plugin and Content Manager > Install from File...

Archive layout (KiCad PCM, metadata schema v2 as shipped with KiCad 10):
    metadata.json            package metadata (no download_* fields inside the archive)
    plugins/                 the IPC action plugin (kicad_plugin/): plugin.json, tracemaker_route.py, ...
    plugins/bin/tracemaker   optional (--binary): bundled engine binary, Linux only
    plugins/bin/tracemaker-place  optional (--place-binary): bundled placer (TRACEMAKER_PLACE)
    plugins/lib/tracemaker*.so  optional (--module): bundled Python module (must match KiCad's Python version)
    plugins/share/tracemaker/viewer/  optional (--viewer DIR): the built live viewer, where the bundled binary looks (../share/tracemaker/viewer)
    resources/icon.png       64x64 package icon (branding/png/tracemaker-icon-64.png)

The archive is reproducible: fixed timestamps, sorted entries. Binaries are never committed; bundle them at
packaging time only.
"""
from __future__ import annotations

import argparse
import hashlib
import io
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PLUGIN_DIR = os.path.join(ROOT, "kicad_plugin")
PLUGIN_FILES = ["plugin.json", "tracemaker_route.py", "requirements.txt", "README.md",
                "icons/tracemaker-24.png", "icons/tracemaker-48.png"]
IDENTIFIER = "org.tracemaker.autoroute"
ZIP_TIME = (2026, 1, 1, 0, 0, 0)


def project_version() -> str:
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        m = re.search(r"project\(TraceMaker VERSION (\d+(?:\.\d+){0,2})", f.read())
    if not m:
        sys.exit("cannot find the project version in CMakeLists.txt")
    return m.group(1)


def git_user() -> str:
    try:
        name = subprocess.run(["git", "config", "user.name"], cwd=ROOT, capture_output=True, text=True).stdout.strip()
    except OSError:
        name = ""
    return name or "TraceMaker developers"


def icon_png() -> bytes:
    """The 64x64 package icon: the brand icon rendered from branding/tracemaker-icon.svg (see branding/README.md)."""
    with open(os.path.join(ROOT, "branding", "png", "tracemaker-icon-64.png"), "rb") as fh:
        return fh.read()


def metadata(version: str, args) -> dict:
    v = {"version": version, "status": args.status, "kicad_version": "10.0", "runtime": "ipc"}
    if args.binary or args.module or args.place_binary:
        v["platforms"] = ["linux"]  # bundled native code
    return {
        "$schema": "https://go.kicad.org/pcm/schemas/v2",
        "name": "TraceMaker",
        "description": "Placement-aware autorouter: routes all unrouted connections of the open board in one undo step",
        "description_full": (
            "TraceMaker routes every unrouted connection of the open board with KiCad's own design rules and adds the "
            "result as a single commit, so one Ctrl-Z removes it. Every committed track and via passes an exact "
            "clearance check.\n\nThe plugin needs the TraceMaker engine: either the `tracemaker` Python module "
            "(TRACEMAKER_PYTHONPATH) or the `tracemaker` binary (TRACEMAKER, or on PATH), unless the package was "
            "built with them bundled. Options go in TRACEMAKER_ARGS, e.g. \"--time 120 --threads 8 --view\"."
        ),
        "identifier": IDENTIFIER,
        "type": "plugin",
        "author": {"name": args.author or git_user(), "contact": {}},
        "license": args.license,
        "resources": {},
        "tags": ["autorouter", "routing", "pcb"],
        "versions": [v],
    }


def read_native(path: str, strip: bool) -> bytes:
    if not strip:
        with open(path, "rb") as fh:
            return fh.read()
    with tempfile.TemporaryDirectory() as tmp:
        out = os.path.join(tmp, os.path.basename(path))
        subprocess.run(["strip", "--strip-debug", "-o", out, path], check=True)
        with open(out, "rb") as fh:
            return fh.read()


def add(z: zipfile.ZipFile, name: str, data: bytes, mode: int = 0o644) -> None:
    info = zipfile.ZipInfo(name, ZIP_TIME)
    info.compress_type = zipfile.ZIP_DEFLATED
    info.external_attr = (0o100000 | mode) << 16
    z.writestr(info, data)


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "pcm"), help="output directory (default build/pcm)")
    ap.add_argument("--version", help="package version (default: the CMake project version)")
    ap.add_argument("--status", default="testing", choices=["stable", "testing", "development", "deprecated"])
    ap.add_argument("--license", default="GPL-3.0-or-later", help="SPDX licence identifier (default GPL-3.0-or-later)")
    ap.add_argument("--author", help="author name (default: git user.name)")
    ap.add_argument("--binary", help="bundle this tracemaker binary as plugins/bin/tracemaker (Linux only)")
    ap.add_argument("--place-binary", help="bundle this tracemaker-place binary as plugins/bin/tracemaker-place (Linux only)")
    ap.add_argument("--module", help="bundle this tracemaker Python module (.so) under plugins/lib/")
    ap.add_argument("--viewer", help="bundle this built viewer directory (viewer/dist) as plugins/share/tracemaker/viewer/")
    ap.add_argument("--strip", action="store_true", help="strip debug information from bundled binaries (needs `strip`)")
    args = ap.parse_args()

    version = args.version or project_version()
    entries: dict[str, tuple[bytes, int]] = {}
    for f in PLUGIN_FILES:
        with open(os.path.join(PLUGIN_DIR, f), "rb") as fh:
            entries[f"plugins/{f}"] = (fh.read(), 0o644)
    if args.binary:
        entries["plugins/bin/tracemaker"] = (read_native(args.binary, args.strip), 0o755)
    if args.place_binary:
        entries["plugins/bin/tracemaker-place"] = (read_native(args.place_binary, args.strip), 0o755)
    if args.module:
        entries[f"plugins/lib/{os.path.basename(args.module)}"] = (read_native(args.module, args.strip), 0o755)
    if args.viewer:
        for dirpath, _, files in os.walk(args.viewer):
            for name in files:
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, args.viewer).replace(os.sep, "/")
                if rel.endswith(".map"):
                    continue  # source maps are 3x the viewer and only for developers
                with open(full, "rb") as fh:
                    entries[f"plugins/share/tracemaker/viewer/{rel}"] = (fh.read(), 0o644)
    entries["resources/icon.png"] = (icon_png(), 0o644)
    entries["metadata.json"] = (json.dumps(metadata(version, args), indent=2).encode() + b"\n", 0o644)

    os.makedirs(args.out, exist_ok=True)
    path = os.path.join(args.out, f"tracemaker-{version}.zip")
    buf = io.BytesIO()
    with zipfile.ZipFile(buf, "w") as z:
        for name in sorted(entries):
            add(z, name, *entries[name])
    data = buf.getvalue()
    with open(path, "wb") as f:
        f.write(data)
    install_size = sum(len(d) for d, _ in entries.values())
    print(path)
    # The fields a PCM repository's packages.json needs for this version (not part of the archive).
    print(json.dumps({"download_sha256": hashlib.sha256(data).hexdigest(), "download_size": len(data), "install_size": install_size}))
    return 0


if __name__ == "__main__":
    sys.exit(main())
