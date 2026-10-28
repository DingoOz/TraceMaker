#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""KiCad version drift: does this build still read, write and judge like the newest KiCad? (M19, doc 18 A4, D142)

For each board: the newest kicad-cli upgrades a copy to its own file format (`kicad-cli pcb upgrade`), then
  1. round trip: `tracemaker check-roundtrip` parses and re-writes the upgraded file byte for byte;
  2. DRC parity: `tracemaker drc` and `kicad-cli pcb drc` count the same violations of the routing-relevant types;
  3. route and judge: `tracemaker route` writes a routed copy that kicad-cli opens and finds no error type the input did not have.

  scripts/kicad_drift.py --kicad-cli kicad-cli --tracemaker build/cpu-only/src/app/tracemaker --out build/drift BOARD ...
  TRACEMAKER_KICAD_IMAGE=kicad/kicad:nightly scripts/kicad_drift.py --kicad-cli scripts/kicad-cli ...   (on a machine with docker)

Writes OUT/drift.json and OUT/drift.md; exit 0 only when every check of every board passed. The work directory must be one kicad-cli
can read (with the docker wrapper: under your home directory, not /tmp).
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import shlex
import shutil
import subprocess
import sys
from collections import Counter

ROOT = pathlib.Path(__file__).resolve().parent.parent
TYPES = ["clearance", "shorting_items", "tracks_crossing", "track_width", "via_diameter", "annular_width", "drill_out_of_range", "hole_clearance",
         "hole_to_hole", "holes_co_located", "copper_edge_clearance", "items_not_allowed", "unconnected_items", "track_dangling", "via_dangling"]


def run(cmd: list[str], timeout: int) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, timeout=timeout)


def counts(report: pathlib.Path) -> Counter:
    d = json.loads(report.read_text())
    c = Counter(v["type"] for v in d.get("violations", []))
    c["unconnected_items"] = len(d.get("unconnected_items", []))
    return c


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("boards", nargs="+")
    ap.add_argument("--kicad-cli", default="kicad-cli", help="kicad-cli command (may be a wrapper with arguments)")
    ap.add_argument("--tracemaker", default=str(ROOT / "build/release/src/app/tracemaker"))
    ap.add_argument("--out", default=str(ROOT / "build/drift"))
    ap.add_argument("--work", type=int, default=2_000_000, help="route work per board")
    ap.add_argument("--timeout", type=int, default=900)
    a = ap.parse_args()
    kc = shlex.split(a.kicad_cli)
    tm = a.tracemaker
    out = pathlib.Path(a.out).resolve()
    out.mkdir(parents=True, exist_ok=True)
    ver = run(kc + ["version"], 300).stdout.strip()
    rows, ok_all = [], True
    for i, src in enumerate(map(pathlib.Path, a.boards)):
        d = out / f"{i:02d}_{src.parent.name}"
        shutil.rmtree(d, ignore_errors=True)
        d.mkdir(parents=True)
        b = d / src.name
        shutil.copy2(src, b)
        for side in src.parent.glob(src.stem + ".kicad_*"):  # the project and rules files
            if side.suffix != ".kicad_pcb":
                shutil.copy2(side, d / side.name)
        row: dict = {"board": str(src), "checks": {}}
        up = run(kc + ["pcb", "upgrade", "--force", str(b)], a.timeout)
        head = b.read_text(errors="replace")[:200]
        m = re.search(r"\(version (\d+)\)", head)
        row["format"] = int(m.group(1)) if m else None
        row["checks"]["upgrade"] = {"ok": up.returncode == 0, "detail": (up.stderr or up.stdout).strip()[-300:]}
        rt = run([tm, "check-roundtrip", str(b)], a.timeout)
        row["checks"]["roundtrip"] = {"ok": rt.returncode == 0, "detail": (rt.stdout + rt.stderr).strip()[-300:]}
        kj, tj = d / "kicad_drc.json", d / "tm_drc.json"
        run(kc + ["pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "-o", str(kj), str(b)], a.timeout)
        run([tm, "drc", str(b), "--json", str(tj)], a.timeout)
        if kj.exists() and tj.exists():
            k, t = counts(kj), counts(tj)
            cap = lambda n: min(n, 199)  # KiCad stops reporting a type after 199 items
            diffs = {ty: [k.get(ty, 0), t.get(ty, 0)] for ty in TYPES if cap(k.get(ty, 0)) != cap(t.get(ty, 0))}
            row["checks"]["drc_parity"] = {"ok": not diffs, "detail": diffs}
        else:
            k = Counter()
            row["checks"]["drc_parity"] = {"ok": False, "detail": "no report from " + ("kicad-cli" if not kj.exists() else "tracemaker")}
        r = d / ("routed_" + src.name)
        for side in (".kicad_pro", ".kicad_dru"):  # `route -o` writes the board only; KiCad reads the rules from the project beside it
            if b.with_suffix(side).exists():
                shutil.copy2(b.with_suffix(side), r.with_suffix(side))
        ro = run([tm, "route", str(b), "-o", str(r), "--work", str(a.work), "--no-gpu", "--no-kb", "--threads", "4"], a.timeout)
        rk = d / "routed_kicad_drc.json"
        if r.exists():
            run(kc + ["pcb", "drc", "--format", "json", "--severity-all", "--all-track-errors", "-o", str(rk), str(r)], a.timeout)
        if rk.exists():
            after = counts(rk)
            added = {ty: [k.get(ty, 0), after[ty]] for ty in after if ty != "unconnected_items" and after[ty] > k.get(ty, 0)}
            row["checks"]["route_judged"] = {"ok": not added, "detail": added or f"unconnected {k.get('unconnected_items', 0)} -> {after['unconnected_items']}"}
        else:
            row["checks"]["route_judged"] = {"ok": False, "detail": ("route exit %d: " % ro.returncode) + (ro.stderr or "").strip()[-300:]}
        row["ok"] = all(c["ok"] for c in row["checks"].values())
        ok_all &= row["ok"]
        rows.append(row)
        print(f"{'PASS' if row['ok'] else 'FAIL'} {src} (format {row['format']}): " + ", ".join(f"{n} {'ok' if c['ok'] else 'FAILED'}" for n, c in row["checks"].items()), flush=True)
    (out / "drift.json").write_text(json.dumps({"kicad": ver, "boards": rows}, indent=1))
    lines = [f"# KiCad drift check against {ver}", "", "| Board | Format | Upgrade | Round trip | DRC parity | Route judged |", "|---|--:|---|---|---|---|"]
    for r in rows:
        c = r["checks"]
        lines.append(f"| {pathlib.Path(r['board']).name} | {r['format']} | " + " | ".join("ok" if c[n]["ok"] else "**failed**" for n in ("upgrade", "roundtrip", "drc_parity", "route_judged")) + " |")
    for r in rows:
        for n, c in r["checks"].items():
            if not c["ok"]:
                lines.append(f"\n- {pathlib.Path(r['board']).name}, {n}: `{json.dumps(c['detail'])[:400]}`")
    (out / "drift.md").write_text("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if ok_all else 1


if __name__ == "__main__":
    sys.exit(main())
