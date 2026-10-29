#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""The release gate (M17, D121): install the release files on a machine with no compiler and route a board.

  scripts/test_release_install.py TARBALL PCM_ZIP [--image ubuntu:24.04] [--work DIR]

Runs a clean container from IMAGE (no compiler, no cmake, no Python: the check below says so), unpacks TARBALL, and
  * `tracemaker version`, `tracemaker-view` serving its web page from the installed layout (../share/tracemaker/viewer);
  * routes the example board (plane_smd) and checks every connection is routed and the DRC finds no error;
  * does it in under five minutes, from unpacking to the checked result.
On the host it checks PCM_ZIP with scripts/validate_pcm.py and that it carries the engine and the viewer. Needs Docker.
Exit 0 = all pass, 1 = a check failed, 77 = no Docker.
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import shutil
import subprocess
import sys
import tempfile
import time
import zipfile

ROOT = pathlib.Path(__file__).resolve().parent.parent
LIMIT_S = 300

# Runs inside the container (as the host user, so the files it leaves can be removed). Prints PASS/FAIL lines and the seconds taken by the install-and-route part.
INSIDE = r"""
set -u
fail=0
check() { if "$@"; then echo "PASS: $DESC"; else echo "FAIL: $DESC"; fail=1; fi; }
for t in g++ gcc cc c++ cmake python3 clang; do
  if command -v $t >/dev/null 2>&1; then echo "FAIL: the image has $t, so it is not a machine without a compiler"; fail=1; fi
done
echo "glibc: $(ldd --version | head -1)"
start=$(date +%s)
mkdir -p /work/unpack && tar xzf /dist/$TARBALL -C /work/unpack || { echo "FAIL: unpacking"; exit 1; }
cd /work/unpack/tracemaker-* && export PATH="$PWD/bin:$PATH"
DESC="tracemaker version"; check tracemaker version
DESC="no shared library beyond libc and libm"; check bash -c '! ldd bin/tracemaker | grep -v "linux-vdso\|libc.so\|libm.so\|ld-linux" | grep -q "=>"'
# the viewer's page, from the installed layout
tracemaker-view share/tracemaker/examples/plane_smd/plane_smd.kicad_pcb --port 18766 >/work/view.log 2>&1 &
vpid=$!
for i in $(seq 1 50); do (exec 3<>/dev/tcp/127.0.0.1/18766) 2>/dev/null && break; sleep 0.2; done
exec 3<>/dev/tcp/127.0.0.1/18766; printf 'GET / HTTP/1.0\r\nHost: 127.0.0.1:18766\r\n\r\n' >&3; cat <&3 | tr -d '\r' > /work/page.txt; exec 3<&-
kill $vpid 2>/dev/null
DESC="the viewer serves index.html from ../share/tracemaker/viewer"; check bash -c "head -1 /work/page.txt | grep -q ' 200 ' && grep -qi '<html' /work/page.txt"
cp share/tracemaker/examples/plane_smd/plane_smd.kicad_pro /work/routed.kicad_pro
tracemaker route share/tracemaker/examples/plane_smd/plane_smd.kicad_pcb -o /work/routed.kicad_pcb --soft-zones --via-in-pad --keep-vias-off-pads \
  --first-nets XIN,XOUT --work 3000000 --variants 4 --threads 2 --no-kb --no-gpu --json /work/route.json >/work/route.log 2>&1
echo "route exit code: $?"
tracemaker drc /work/routed.kicad_pcb --refill-zones --json /work/drc.json >/work/drc.log 2>&1
echo "drc exit code: $?"
end=$(date +%s)
echo "SECONDS: $((end - start))"
exit $fail
"""


def docker_cmd() -> list[str] | None:
    if shutil.which("docker") is None:
        return None
    if subprocess.run(["docker", "info"], capture_output=True).returncode == 0:
        return ["docker"]
    if shutil.which("sg") and subprocess.run(["sg", "docker", "-c", "docker info"], capture_output=True).returncode == 0:
        return ["sg-docker"]  # this shell started before the user joined the docker group
    return None


def run_docker(prefix: list[str], args: list[str]) -> subprocess.CompletedProcess:
    if prefix == ["sg-docker"]:
        import shlex
        return subprocess.run(["sg", "docker", "-c", "docker " + " ".join(shlex.quote(a) for a in args)], capture_output=True, text=True)
    return subprocess.run(prefix + args, capture_output=True, text=True)


def check_pcm(pcm: pathlib.Path) -> list[str]:
    problems = []
    r = subprocess.run([sys.executable, str(ROOT / "scripts" / "validate_pcm.py"), str(pcm)], capture_output=True, text=True)
    if r.returncode != 0:
        problems.append("validate_pcm.py: " + (r.stdout + r.stderr).strip()[-400:])
    with zipfile.ZipFile(pcm) as z:
        names = set(z.namelist())
        for need in ("plugins/bin/tracemaker", "plugins/bin/tracemaker-place", "plugins/share/tracemaker/viewer/index.html", "plugins/tracemaker_route.py", "metadata.json"):
            if need not in names:
                problems.append(f"the plugin package has no {need}")
        meta = json.loads(z.read("metadata.json"))
        if meta["versions"][0].get("platforms") != ["linux"]:
            problems.append("the plugin package is not marked for linux")
    return problems


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("tarball", type=pathlib.Path)
    ap.add_argument("pcm", type=pathlib.Path)
    ap.add_argument("--image", default="ubuntu:24.04")
    ap.add_argument("--work", type=pathlib.Path, help="directory for the container's files (default: a temporary one)")
    args = ap.parse_args()
    docker = docker_cmd()
    if docker is None:
        print("SKIP: no usable Docker")
        return 77
    failures: list[str] = []
    tarball = args.tarball.resolve()
    with tempfile.TemporaryDirectory(dir=ROOT / "build") as tmp:
        work = (args.work or pathlib.Path(tmp)).resolve()
        work.mkdir(parents=True, exist_ok=True)
        t0 = time.time()
        r = run_docker(docker, ["run", "--rm", "-u", f"{os.getuid()}:{os.getgid()}", "-v", f"{tarball.parent}:/dist:ro", "-v", f"{work}:/work", "-e", f"TARBALL={tarball.name}", args.image, "bash", "-c", INSIDE])
        wall = time.time() - t0
        print(r.stdout, end="")
        if r.stderr.strip():
            print(r.stderr.strip()[-800:])
        if r.returncode != 0:
            failures.append(f"the container checks failed (exit code {r.returncode})")
        try:
            route = json.loads((work / "route.json").read_text())
            drc = json.loads((work / "drc.json").read_text())
            n_conn, n_routed = route.get("connections"), route.get("routed")
            errors = [v for v in drc.get("violations", []) if v.get("severity") == "error"]
            print(f"routed {n_routed} of {n_conn} connections; DRC errors: {len(errors)}; unconnected: {len(drc.get('unconnected_items', []))}")
            if n_conn is None or n_routed != n_conn:
                failures.append(f"routed {n_routed} of {n_conn} connections")
            if errors:
                failures.append(f"{len(errors)} DRC errors on the routed example")
            if drc.get("unconnected_items"):
                failures.append(f"{len(drc['unconnected_items'])} unconnected items on the routed example")
        except (OSError, ValueError) as e:
            failures.append(f"no route/DRC result from the container: {e}")
        secs = [int(line.split()[1]) for line in r.stdout.splitlines() if line.startswith("SECONDS:")]
        print(f"install and route took {secs[0] if secs else '?'} s inside the container ({wall:.0f} s with the container start)")
        if not secs or secs[0] > LIMIT_S:
            failures.append(f"over the {LIMIT_S} s limit")
    failures += check_pcm(args.pcm.resolve())
    print("PASS: the plugin package validates and carries the engine and the viewer" if not any("plugin" in f or "validate" in f for f in failures) else "FAIL: the plugin package")
    for f in failures:
        print("FAIL:", f)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
