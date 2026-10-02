#!/usr/bin/env python3
"""Checks that the benchmark harness can reach its external tools.

Exit code 0 when everything required is present. Optional tools are reported but not required.
"""
from __future__ import annotations

import pathlib
import re
import shutil
import subprocess
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
FREEROUTING_CANDIDATES = [
    ROOT.parent / "pcbgolf/tools/fr/freerouting-2.4.1-linux-x64/bin/freerouting",
]


def run(cmd: list[str], timeout: float = 120) -> tuple[int, str]:
    try:
        p = subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)
        return p.returncode, (p.stdout + p.stderr).strip()
    except (OSError, subprocess.SubprocessError) as e:
        return 1, str(e)


def main() -> int:
    ok = True
    # Required: kicad-cli 10.x (the DRC judge).
    code, out = run(["kicad-cli", "version"])
    m = re.search(r"(\d+)\.(\d+)\.(\d+)", out.splitlines()[-1] if out else "")
    if code == 0 and m and int(m.group(1)) >= 10:
        print(f"OK    kicad-cli {m.group(0)}")
    else:
        print(f"FAIL  kicad-cli 10.x not usable: {out[-200:]}")
        ok = False
    # Required: the engine binary (any preset).
    exes = sorted(ROOT.glob("build/*/src/app/tracemaker"))
    if exes:
        code, out = run([str(exes[0]), "version"])
        print(f"OK    {out} ({exes[0].relative_to(ROOT)})" if code == 0 else f"FAIL  {exes[0]}: {out}")
        ok &= code == 0
    else:
        print("FAIL  no tracemaker binary; run scripts/bootstrap.sh")
        ok = False
    # Optional: Freerouting baseline.
    fr = next((p for p in FREEROUTING_CANDIDATES if p.exists()), None) or shutil.which("freerouting")
    print(f"OK    Freerouting baseline at {fr}" if fr else "INFO  Freerouting not found (baseline runs disabled)")
    # Optional: fixtures.
    data = ROOT / "bench" / "data"
    sets = sorted(p.name for p in data.iterdir() if p.is_dir()) if data.is_dir() else []
    print(f"OK    fixture sets: {', '.join(sets)}" if sets else "INFO  no fixtures; run scripts/fetch_fixtures.sh")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
