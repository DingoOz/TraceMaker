#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Builds the release files (D121): a tarball with the binaries, the viewer, an example board and the agent skills,
and the KiCad plugin package (PCM zip) with the engine bundled.

  scripts/make_release.py [--out DIR] [--version V] [--skip-build] [--build-viewer]

Output in DIR (default build/release-files):
  tracemaker-V-<os>-<arch>.tar.gz      bin/{tracemaker,tracemaker-place,tracemaker-view}, share/tracemaker/{viewer,examples,skills}, INSTALL.txt
  tracemaker-V-pcm-<os>-<arch>.zip     the KiCad 10 plugin (Plugin and Content Manager > Install from File...) with bin/tracemaker bundled
  SHA256SUMS

The binaries come from the `dist` preset: CPU only, libstdc++, libgcc, SQLite and zstd linked statically, so the only
runtime requirement is glibc (the build host's, which names the oldest system it runs on: INSTALL.txt says which).
Both archives are reproducible for a given build (fixed timestamps, sorted entries). Needs cmake, ninja, g++-15 and,
with --build-viewer, node and npm (the built viewer, viewer/dist, is not in the repository).
"""
from __future__ import annotations

import argparse
import gzip
import hashlib
import io
import os
import platform
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FIXED_TIME = 1767225600  # 2026-01-01T00:00:00Z


def project_version() -> str:
    with open(os.path.join(ROOT, "CMakeLists.txt")) as f:
        m = re.search(r"project\(TraceMaker VERSION (\d+(?:\.\d+){0,2})", f.read())
    if not m:
        sys.exit("cannot find the project version in CMakeLists.txt")
    return m.group(1)


def run(cmd: list[str], **kw) -> None:
    print("+", " ".join(cmd), flush=True)
    subprocess.run(cmd, check=True, **kw)


def glibc_floor(binary: str) -> str:
    """The newest GLIBC_x.y symbol version the binary asks for: the oldest glibc it runs on."""
    out = subprocess.run(["objdump", "-T", binary], capture_output=True, text=True, check=True).stdout
    versions = {tuple(int(x) for x in m.split(".")) for m in re.findall(r"GLIBC_(\d+(?:\.\d+)+)", out)}
    return ".".join(map(str, max(versions))) if versions else "unknown"


def install_text(version: str, glibc: str, os_name: str, arch: str) -> str:
    return f"""TraceMaker {version} ({os_name} {arch})

This archive holds the command line tools, the live viewer, an example board and the agent skills. Nothing else is needed:
the programs carry their own C++ runtime, SQLite and zstd. They need glibc {glibc} or newer; `ldd --version` shows yours.
Tried: Ubuntu 24.04, Debian 13 and the current Fedora run them; Debian 12 (glibc 2.36) does not, and older systems have older glibc still.

  bin/tracemaker          route, check (DRC), inspect, session (for agents) ... : `tracemaker --help`
  bin/tracemaker-place    placement
  bin/tracemaker-view     the live viewer on its own, to replay a log
  share/tracemaker/viewer the viewer's web page (found by the binaries; TM_VIEW_DIR overrides)
  share/tracemaker/examples/plane_smd   a small board with planes and a BGA
  share/tracemaker/skills agent skills (Claude Code and similar): copy a folder into your skills directory

Try it:

  export PATH="$PWD/bin:$PATH"
  tracemaker version
  tracemaker route share/tracemaker/examples/plane_smd/plane_smd.kicad_pcb -o /tmp/routed.kicad_pcb \\
      --soft-zones --via-in-pad --keep-vias-off-pads --first-nets XIN,XOUT --work 3000000 --variants 2 --no-gpu
  tracemaker drc /tmp/routed.kicad_pcb

(A board's .kicad_pro, next to the .kicad_pcb, gives the design rules: copy it beside the output.)

The KiCad plugin is the other file of the release, tracemaker-{version}-pcm-{os_name}-{arch}.zip: in KiCad 10, enable
Preferences > Plugins > "Enable KiCad API", then Plugin and Content Manager > Install from File... > choose the zip. It
carries its own copy of bin/tracemaker.

Licence: GPL-3.0-or-later (share/doc/tracemaker/LICENSE; third-party notices in NOTICE).
"""


def add_file(tar: tarfile.TarFile, name: str, data: bytes, mode: int) -> None:
    info = tarfile.TarInfo(name)
    info.size, info.mode, info.mtime, info.uid, info.gid, info.uname, info.gname = len(data), mode, FIXED_TIME, 0, 0, "root", "root"
    tar.addfile(info, io.BytesIO(data))


def make_tarball(stage: str, top: str, path: str, extra: dict[str, bytes]) -> None:
    entries: list[tuple[str, str]] = []
    for dirpath, _, files in os.walk(stage):
        for f in files:
            full = os.path.join(dirpath, f)
            entries.append((os.path.relpath(full, stage).replace(os.sep, "/"), full))
    raw = io.BytesIO()
    with tarfile.open(fileobj=raw, mode="w", format=tarfile.PAX_FORMAT) as tar:
        for rel, full in sorted(entries):
            with open(full, "rb") as fh:
                add_file(tar, f"{top}/{rel}", fh.read(), 0o755 if os.access(full, os.X_OK) else 0o644)
        for rel, data in sorted(extra.items()):
            add_file(tar, f"{top}/{rel}", data, 0o644)
    with open(path, "wb") as out:
        with gzip.GzipFile(filename="", mode="wb", fileobj=out, mtime=0, compresslevel=9) as gz:
            gz.write(raw.getvalue())


def sha256(path: str) -> str:
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--out", default=os.path.join(ROOT, "build", "release-files"))
    ap.add_argument("--version", help="release version (default: the CMake project version)")
    ap.add_argument("--skip-build", action="store_true", help="use the existing build/dist")
    ap.add_argument("--build-viewer", action="store_true", help="npm ci && npm run build in viewer/ first")
    args = ap.parse_args()
    version = args.version or project_version()
    os_name = {"linux": "linux", "darwin": "macos"}.get(sys.platform, sys.platform)
    arch = {"x86_64": "x86_64", "AMD64": "x86_64", "arm64": "arm64", "aarch64": "arm64"}.get(platform.machine(), platform.machine())

    viewer = os.path.join(ROOT, "viewer")
    if args.build_viewer:
        run(["npm", "ci", "--no-audit", "--no-fund"], cwd=viewer)
        run(["npm", "run", "build"], cwd=viewer)
    if not os.path.isfile(os.path.join(viewer, "dist", "index.html")):
        sys.exit("viewer/dist/index.html is missing: run with --build-viewer (needs node and npm)")
    if not args.skip_build:
        run(["cmake", "--preset", "dist"], cwd=ROOT)
        run(["cmake", "--build", "--preset", "dist"], cwd=ROOT)

    os.makedirs(args.out, exist_ok=True)
    top = f"tracemaker-{version}-{os_name}-{arch}"
    with tempfile.TemporaryDirectory(dir=os.path.join(ROOT, "build")) as tmp:
        stage = os.path.join(tmp, top)
        run(["cmake", "--install", os.path.join(ROOT, "build", "dist"), "--prefix", stage, "--strip"], cwd=ROOT)
        binary = os.path.join(stage, "bin", "tracemaker")
        glibc = glibc_floor(binary) if os_name == "linux" else "n/a"
        tarball = os.path.join(args.out, f"{top}.tar.gz")
        make_tarball(stage, top, tarball, {"INSTALL.txt": install_text(version, glibc, os_name, arch).encode()})
        pcm_dir = os.path.join(tmp, "pcm")
        run([sys.executable, os.path.join(ROOT, "scripts", "make_pcm_package.py"), "--out", pcm_dir, "--version", version, "--binary", binary, "--place-binary", os.path.join(stage, "bin", "tracemaker-place"),
             "--viewer", os.path.join(stage, "share", "tracemaker", "viewer")], cwd=ROOT)
        pcm = os.path.join(args.out, f"tracemaker-{version}-pcm-{os_name}-{arch}.zip")
        shutil.copy(os.path.join(pcm_dir, f"tracemaker-{version}.zip"), pcm)
    with open(os.path.join(args.out, "SHA256SUMS"), "w") as f:
        for p in (tarball, pcm):
            f.write(f"{sha256(p)}  {os.path.basename(p)}\n")
    print(f"glibc floor of the binary: {glibc}")
    for p in (tarball, pcm):
        print(f"{p}  {os.path.getsize(p) / 1e6:.1f} MB")
    return 0


if __name__ == "__main__":
    sys.exit(main())
