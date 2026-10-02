#!/usr/bin/env python3
"""Route one PCBench board with TraceMaker and Freerouting versions, then measure quality the same way for all.

  bench/compare.py AmpOne_dev-AmpOne [--fr 2.5.0-RC12 1.9.0] [--tm-time 120] [--fr-timeout 00:30:00] [--out DIR]

Freerouting runs with its own published benchmark settings (one thread, up to 500 passes, fanout and optimizer on,
job timeout 30 min), from the Specctra DSN in the fixture; its session is imported into the KiCad board
(bench/ses_import.py). TraceMaker routes the .kicad_pcb directly. The human-routed original (raw.kicad_pcb) is
measured as a reference. Every result is judged by kicad-cli DRC and bench/quality.py, and wall time is recorded.
"""
import argparse
import json
import os
import pathlib
import shutil
import subprocess
import time

ROOT = pathlib.Path(__file__).resolve().parent.parent
FRB = ROOT / "bench/data/freerouting/scripts/benchmark"
FIX = FRB / "fixtures/PCBench"
JAVA = next(iter(sorted((ROOT / "build/tools").glob("jdk-25*/bin/java"))), pathlib.Path("java"))
TM = ROOT / "build/release/src/app/tracemaker"


def run_fr(version: str, dsn: pathlib.Path, ses: pathlib.Path, timeout: str, log: pathlib.Path) -> dict:
    jar = FRB / "binaries" / f"freerouting-{version}.jar"
    cmd = [str(JAVA), "-Xmx8g", "-jar", str(jar), "-de", str(dsn), "-do", str(ses), "--router.max_threads=1",
           f"--router.job_timeout={timeout}", "--router.autorouter.max_passes=500", "--router.optimizer.enabled=true",
           "--router.fanout.enabled=true"]
    if version.startswith("1."):
        # v1.9 opens a window even in CLI mode: needs a (virtual) display.
        if not os.environ.get("DISPLAY"):
            if not shutil.which("xvfb-run"):
                return {"error": "Freerouting 1.9 needs a display: install xvfb (/tmp/tracemaker_xvfb_setup.sh)"}
            cmd = ["xvfb-run", "-a"] + cmd
    else:
        cmd.insert(1, "-Djava.awt.headless=true")
        cmd += ["--api_server.enabled=false", "--gui.enabled=false"]
    t0 = time.time()
    with open(log, "w") as f:
        p = subprocess.run(cmd, stdout=f, stderr=subprocess.STDOUT)
    wall = time.time() - t0
    if p.returncode != 0 or not ses.exists():
        return {"error": f"exit {p.returncode}", "wall_s": round(wall, 1)}
    return {"wall_s": round(wall, 1)}


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("--fr", nargs="*", default=["2.5.0-RC12", "1.9.0"])
    ap.add_argument("--tm-time", type=float, default=120)
    ap.add_argument("--tm-threads", type=int, default=8)
    ap.add_argument("--fr-timeout", default="00:30:00")
    ap.add_argument("--out", default=str(ROOT / "build/compare"))
    a = ap.parse_args()
    src = FIX / a.board
    out = pathlib.Path(a.out) / a.board
    out.mkdir(parents=True, exist_ok=True)
    unrouted = src / "unrouted.kicad_pcb"
    runs = {}

    human = out / "human.kicad_pcb"
    shutil.copy(src / "raw.kicad_pcb", human)
    runs["Human original"] = {"file": human, "wall_s": None}

    tm_out = out / "tracemaker.kicad_pcb"
    t0 = time.time()
    p = subprocess.run([str(TM), "route", str(unrouted), "-o", str(tm_out), "--time", str(a.tm_time), "--threads",
                        str(a.tm_threads), "--no-kb", "--json", str(tm_out) + ".route.json"], capture_output=True, text=True)
    runs["TraceMaker"] = {"file": tm_out, "wall_s": round(time.time() - t0, 1)}
    if p.returncode not in (0, 3):
        runs["TraceMaker"]["error"] = p.stderr[-300:]

    for v in a.fr:
        ses = out / f"fr-{v}.ses"
        info = run_fr(v, src / "unrouted.dsn", ses, a.fr_timeout, out / f"fr-{v}.log")
        board = out / f"fr-{v}.kicad_pcb"
        if "error" not in info:
            subprocess.run(["python3", str(ROOT / "bench/ses_import.py"), str(unrouted), str(ses), "-o", str(board)], check=True,
                           capture_output=True)
            info["file"] = board
        runs[f"Freerouting {v}"] = info

    import importlib.util
    spec = importlib.util.spec_from_file_location("quality", ROOT / "bench/quality.py")
    q = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(q)
    rows = []
    for label, info in runs.items():
        row = {"label": label, "wall_s": info.get("wall_s")}
        if "file" in info:
            row.update(q.metrics(unrouted, info["file"]))
            row["file"] = str(info["file"])
        else:
            row["error"] = info.get("error")
        rows.append(row)
        print(label, {k: row.get(k) for k in ("completion", "clean", "router_errors", "length_mm", "vias", "bends", "sharp_bends",
                                             "narrowed_mm", "wall_s", "error")})
    (out / "quality.json").write_text(json.dumps({"board": a.board, "runs": rows}, indent=1))
    print("wrote", out / "quality.json")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
