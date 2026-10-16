#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The plugin package with the engine inside (M17, D121): make_pcm_package.py --binary --place-binary --viewer.

  pcm_bundled.py <tracemaker> <tracemaker-place> <work dir>

Builds the package from the two real binaries and a one-file viewer folder, and checks that the archive validates, is marked for Linux,
holds the router, the placer and the viewer where the plugin and the binaries look for them (plugins/bin, plugins/share/tracemaker/viewer),
keeps the executable bit of the binaries in the archive's attributes, and leaves source maps out.
"""
import json
import pathlib
import subprocess
import sys
import zipfile

ROOT = pathlib.Path(__file__).resolve().parents[2]


def main() -> int:
    tm, place, work = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
    viewer = work / "viewer"
    (viewer / "assets").mkdir(parents=True, exist_ok=True)
    (viewer / "index.html").write_text("<html></html>")
    (viewer / "assets" / "app.js").write_text("//")
    (viewer / "assets" / "app.js.map").write_text("{}")
    out = work / "pcm"
    subprocess.run([sys.executable, str(ROOT / "scripts/make_pcm_package.py"), "--out", str(out), "--version", "9.9.9", "--binary", tm, "--place-binary", place,
                    "--viewer", str(viewer)], check=True, capture_output=True)
    pkg = out / "tracemaker-9.9.9.zip"
    r = subprocess.run([sys.executable, str(ROOT / "scripts/validate_pcm.py"), str(pkg)], capture_output=True, text=True)
    bad = []
    if r.returncode != 0:
        bad.append("validate_pcm.py: " + (r.stdout + r.stderr)[-300:])
    with zipfile.ZipFile(pkg) as z:
        names = set(z.namelist())
        for need in ("plugins/bin/tracemaker", "plugins/bin/tracemaker-place", "plugins/share/tracemaker/viewer/index.html", "plugins/share/tracemaker/viewer/assets/app.js"):
            if need not in names:
                bad.append(f"missing {need}")
        if "plugins/share/tracemaker/viewer/assets/app.js.map" in names:
            bad.append("a source map was bundled")
        for exe in ("plugins/bin/tracemaker", "plugins/bin/tracemaker-place"):
            if exe in names and not (z.getinfo(exe).external_attr >> 16) & 0o111:
                bad.append(f"{exe} has no executable bit")
        if json.loads(z.read("metadata.json"))["versions"][0].get("platforms") != ["linux"]:
            bad.append("the package is not marked for linux")
    for b in bad:
        print("FAIL:", b)
    if not bad:
        print("PASS: the bundled package validates and has the engine, the placer and the viewer")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
