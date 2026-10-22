#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live status page for the real-world routing study (reports/realworld/): the rule-escalation ladder run on each board.

Reads reports/realworld/boards/<board>/<run>/stage-*/route.log (the router's own result line, one per stage), <run>/ledger.json (KiCad's verdict and
the compromise ledger), reports/realworld/status.json (hand-edited: which boards are under test, what each run name means, findings), running processes.
Read-only; standard library only. Styled like the M14 page.

Usage: devsite/realworld.py [--host 0.0.0.0] [--port 8770]
"""
from __future__ import annotations

import argparse
import json
import pathlib
import re
import math
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
STATIC = pathlib.Path(__file__).resolve().parent / "static"
RW = ROOT / "reports" / "realworld"
BOARDS = RW / "boards"
sys.path.insert(0, str(RW / "py"))
LAYER_COL = {"F.Cu": "#ef5b5b", "B.Cu": "#5b9cf5", "In1.Cu": "#3fbf6a", "In2.Cu": "#d8b13a"}
UNROUTED_RE = re.compile(r"^\s+unrouted: (.*?): (\S+) -> (\S+?)\s+\(")
ROUTED_RE = re.compile(r"^routed (\d+)/(\d+) connections, (\d+) tracks, (\d+) vias, pitch ([0-9.]+) mm, (\d+) expansions, ([0-9.]+) s")
_cache: dict[str, tuple[float, object]] = {}
_lock = threading.Lock()


def cached(key: str, ttl: float, fn):
    now = time.time()
    with _lock:
        hit = _cache.get(key)
        if hit and now - hit[0] < ttl:
            return hit[1]
    v = fn()
    with _lock:
        _cache[key] = (now, v)
    return v


def sh(cmd: list[str], timeout: float = 8.0) -> str:
    try:
        return subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True, timeout=timeout).stdout
    except (OSError, subprocess.SubprocessError):
        return ""


def read_json(p: pathlib.Path, default):
    try:
        return json.loads(p.read_text())
    except (OSError, ValueError):
        return default


def run_source(run_dir: pathlib.Path) -> tuple[pathlib.Path | None, pathlib.Path | None]:
    """The newest copper of a run (last finished stage, or the designer's routing) and the route.log that goes with it."""
    if run_dir.name == "human":
        return run_dir / "routed.kicad_pcb", None
    best = None
    for s in sorted(run_dir.glob("stage-*")):
        if (s / "routed_relaxed.kicad_pcb").exists():
            best = s
    if best:
        return best / "routed_relaxed.kicad_pcb", best / "route.log"
    return run_dir.parent / "planes.kicad_pcb", None


def open_lines(board, run_dir: pathlib.Path, log: pathlib.Path | None) -> list[tuple[float, float, float, float]]:
    """Where connections are still open: KiCad's list when the run has been judged, else the router's unrouted list (pad to pad)."""
    drc = read_json(run_dir / "routed.drc.json", None)
    if drc is not None and "unconnected_items" in drc:
        out = []
        for u in drc["unconnected_items"]:
            # a pour (zone) is reported at its anchor corner, not where it falls short: mark only the other end, as a ring
            pts = [(i["pos"]["x"], i["pos"]["y"]) for i in u["items"] if not i["description"].startswith("Zone")]
            if len(pts) >= 2:
                out.append((pts[0][0], pts[0][1], pts[1][0], pts[1][1]))
            elif pts:
                out.append((pts[0][0], pts[0][1], pts[0][0], pts[0][1]))
        return out
    if log is None:
        return []
    pads = {}
    for part in board.parts:
        for p in part.pads:
            pads.setdefault(f"{part.ref}.{p.num}", (p.x, p.y))
    out = []
    try:
        for l in log.read_text(errors="replace").splitlines():
            m = UNROUTED_RE.match(l)
            if m and m[2] in pads and m[3] in pads:
                out.append((*pads[m[2]], *pads[m[3]]))
    except OSError:
        pass
    return out


def render_svg(board_name: str, run: str) -> bytes:
    """The board as the run left it, drawn from the .kicad_pcb: copper by layer, pads, vias, open connections as dashed magenta lines with rings."""
    import kgeom
    run_dir = BOARDS / board_name / run
    src, log = run_source(run_dir)
    if src is None or not src.exists():
        return b""
    b = kgeom.load(str(src))
    xs, ys = [], []
    for e in b.edge:
        xs += [e[0], e[2]]
        ys += [e[1], e[3]]
    for part in b.parts:
        xs.append(part.x)
        ys.append(part.y)
    x0, y0, x1, y1 = min(xs) - 2, min(ys) - 2, max(xs) + 2, max(ys) + 2
    o = [f'<svg xmlns="http://www.w3.org/2000/svg" viewBox="{x0:.2f} {y0:.2f} {x1 - x0:.2f} {y1 - y0:.2f}" preserveAspectRatio="xMidYMid meet">',
         f'<rect x="{x0:.2f}" y="{y0:.2f}" width="{x1 - x0:.2f}" height="{y1 - y0:.2f}" fill="#0e1624"/>']
    o.append('<g stroke="#8a97ad" stroke-width="0.25" fill="none">' + "".join(f'<line x1="{e[0]:.2f}" y1="{e[1]:.2f}" x2="{e[2]:.2f}" y2="{e[3]:.2f}"/>' for e in b.edge) + "</g>")
    order = ["B.Cu", "In2.Cu", "In1.Cu", "F.Cu"]
    for layer in order[:3]:
        segs = [t for t in b.tracks if t.layer == layer]
        if segs:
            o.append(f'<g stroke="{LAYER_COL.get(layer, "#888")}" stroke-linecap="round" opacity="0.75">' + "".join(
                f'<line x1="{t.x1:.2f}" y1="{t.y1:.2f}" x2="{t.x2:.2f}" y2="{t.y2:.2f}" stroke-width="{max(t.w, 0.15):.3f}"/>' for t in segs) + "</g>")
    pad_svg = []
    for part in b.parts:
        for p in part.pads:
            col = "#c9a227" if p.kind == "thru_hole" else ("#ef5b5b" if any(l.startswith("F.") or l == "*.Cu" for l in p.layers) else "#5b9cf5")
            if p.shape == "circle" or p.kind == "np_thru_hole":
                pad_svg.append(f'<circle cx="{p.x:.2f}" cy="{p.y:.2f}" r="{min(p.w, p.h) / 2:.2f}" fill="{col}" opacity="0.55"/>')
            else:
                c, s = math.cos(math.radians(-p.rot)), math.sin(math.radians(-p.rot))
                pts = " ".join(f"{p.x + lx * c - ly * s:.2f},{p.y + lx * s + ly * c:.2f}" for lx, ly in ((-p.w / 2, -p.h / 2), (p.w / 2, -p.h / 2), (p.w / 2, p.h / 2), (-p.w / 2, p.h / 2)))
                pad_svg.append(f'<polygon points="{pts}" fill="{col}" opacity="0.55"/>')
    o.append("<g>" + "".join(pad_svg) + "</g>")
    segs = [t for t in b.tracks if t.layer == "F.Cu"]
    o.append(f'<g stroke="{LAYER_COL["F.Cu"]}" stroke-linecap="round" opacity="0.8">' + "".join(
        f'<line x1="{t.x1:.2f}" y1="{t.y1:.2f}" x2="{t.x2:.2f}" y2="{t.y2:.2f}" stroke-width="{max(t.w, 0.15):.3f}"/>' for t in segs) + "</g>")
    o.append('<g fill="#dfe6f2" stroke="#0e1624" stroke-width="0.08">' + "".join(f'<circle cx="{v.x:.2f}" cy="{v.y:.2f}" r="{v.size / 2:.2f}"/>' for v in b.vias) + "</g>")
    ol = open_lines(b, run_dir, log)
    span = max(x1 - x0, y1 - y0)
    if ol:
        o.append(f'<g stroke="#ff2bd6" fill="none" stroke-width="{span / 220:.2f}">' + "".join(
            (f'<line x1="{a:.2f}" y1="{bb:.2f}" x2="{c:.2f}" y2="{d:.2f}" stroke-dasharray="{span / 90:.2f} {span / 140:.2f}"/>' if (a, bb) != (c, d) else "")
            + f'<circle cx="{a:.2f}" cy="{bb:.2f}" r="{span / 70:.2f}"/><circle cx="{c:.2f}" cy="{d:.2f}" r="{span / 70:.2f}"/>' for a, bb, c, d in ol) + "</g>")
    saf = read_json(safety_file(board_name, run), None)
    flags = (saf or {}).get("flags", [])
    if any(f["sev"] != "ok" for f in flags):
        o.append(f'<rect x="{x0:.2f}" y="{y0:.2f}" width="{x1 - x0:.2f}" height="{y1 - y0:.2f}" fill="#0e1624" opacity="0.55"/>')  # dim the board so what the simulation flags stands out
    SEV = {"fail": "#ff3b3b", "warn": "#ff9f1a", "watch": "#ffd23f", "ok": "#3fbf6a"}
    for f in reversed(flags):  # worst last, so it is on top
        col = SEV.get(f["sev"], "#ffd23f")
        g = [f'<g class="flag {f["sev"]}"><title>{f["text"].replace("&", "&amp;").replace("<", "&lt;")}</title>']
        for sg in f.get("segs", []):
            g.append(f'<line x1="{sg[0]}" y1="{sg[1]}" x2="{sg[2]}" y2="{sg[3]}" stroke="{col}" stroke-width="{max(sg[4], 0.15) + 0.3:.3f}" stroke-linecap="round" opacity="0.95"/>')
        r = span / (110 if f["sev"] == "ok" else 60)
        for pt in f.get("pts", []):
            g.append(f'<circle cx="{pt[0]}" cy="{pt[1]}" r="{r:.2f}" fill="none" stroke="{col}" stroke-width="{span / 300:.2f}" opacity="{0.5 if f["sev"] == "ok" else 1}"/>')
        g.append("</g>")
        o.append("".join(g))
    o.append("</svg>")
    return "".join(o).encode()


def stage_info(d: pathlib.Path, alive: bool) -> dict:
    log = d / "route.log"
    tier = d.name.split("-", 2)[2] if d.name.count("-") >= 2 else d.name
    info = {"stage": d.name, "tier": tier, "state": "waiting"}
    try:  # the run's stage list (SIGNAL or SIGNAL/POWER tiers): label this stage and say how far the power nets have been relaxed
        labels = (d.parent / "stages.txt").read_text().strip().split(",")
        lab = labels[int(d.name.split("-")[1])]
        info["label"] = lab
        info["power"] = lab.partition("/")[2] or None
    except (OSError, ValueError, IndexError):
        pass
    try:
        lines = log.read_text(errors="replace").splitlines()
        mt = log.stat().st_mtime
    except OSError:
        return info
    for l in lines:
        m = ROUTED_RE.match(l)
        if m:
            info.update(state="done", routed=int(m[1]), total=int(m[2]), tracks=int(m[3]), vias=int(m[4]), pitch=float(m[5]), expansions=int(m[6]), seconds=float(m[7]))
            return info
    variants = [l.strip() for l in lines if l.lstrip().startswith("variant")]
    info.update(state="running" if alive and time.time() - mt < 600 else "stopped", age=time.time() - mt, variants=len(variants),
                best=max((int(re.search(r"routed (\d+)", v)[1]) for v in variants if re.search(r"routed (\d+)", v)), default=None))
    return info


_power: dict[str, set] = {}
POWER_ITEM = re.compile(r"^(Track|Via|Pad.*?|PTH pad.*?) \[([^\]]*)\]")


def power_nets_of(board: str) -> set:
    if board not in _power:
        try:
            import rules
            _power[board] = set(rules.power_nets((BOARDS / board / "planes.kicad_pcb").read_text()))
        except Exception:
            _power[board] = set()
    return _power[board]


def ledger_info(p: pathlib.Path, board: str = "") -> dict | None:
    j = read_json(p, None)
    if not j:
        return None
    pw = power_nets_of(board) if board else set()
    on_power = sum(1 for a in j.get("added", []) if a["type"] in ("clearance", "track_width", "drill_out_of_range", "via_diameter", "annular_width")
                   and any((m := POWER_ITEM.match(d)) and m.group(2) in pw for d in a.get("items", [])))
    classes: dict[str, int] = {}
    gap = None
    for a in j.get("added", []):
        k = f"{a['type']} {a.get('class', '').split(' ')[0]}".strip()
        classes[k] = classes.get(k, 0) + 1
        if a["type"] == "clearance" and a.get("actual") is not None:
            gap = a["actual"] if gap is None else min(gap, a["actual"])
    return {"unconnected": j.get("unconnected"), "baseline_unconnected": j.get("unconnected_baseline"), "added": len(j.get("added", [])),
            "classes": classes, "min_gap": gap, "on_power": on_power, "shorts": sum(1 for a in j.get("added", []) if a["type"] == "shorting_items" and any(i.startswith(("Track", "Via", "Arc")) for i in a.get("items", []))), "tracks": j.get("tracks"), "vias": j.get("vias")}


def safety_file(board: str, run: str) -> pathlib.Path:
    return RW / "sim" / "results" / board / f"safety.{run}.json"


def safety_info(board: str, run: str) -> dict | None:
    """The compact state of the safety simulation (py/safety.py) of a run, also while it is still running; the full file is served by /api/safety/BOARD/RUN."""
    f = safety_file(board, run)
    j = read_json(f, None)
    if not j:
        return None
    c, i, v = j.get("crosstalk") or {}, j.get("current") or {}, j.get("vias") or {}
    fl = j.get("flags", [])
    prog = c.get("progress") or {}
    return {"status": j.get("status", "done"), "verdict": j.get("verdict") if j.get("status", "done") == "done" else None, "t": f.stat().st_mtime,
            "progress": [prog.get("done", 0), prog.get("total", 0)],
            "flags": {s: sum(1 for x in fl if x["sev"] == s) for s in ("fail", "warn", "watch", "ok")},
            "crosstalk": {"verdict": c.get("verdict"), "screened": c.get("pairs_screened"), "bound": c.get("bound_worst_pct"), "sim": c.get("sim_worst_pct"), "simulated": len(c.get("simulated", []))},
            "current": {"verdict": i.get("verdict"), "narrowed": len(i.get("narrowed_nets", [])), "power": len(i.get("dc", [])), "drop": max([d.get("worst_drop_pct", 0) for d in i.get("dc", [])], default=0)},
            "vias": {"verdict": v.get("verdict"), "pct": v.get("pct_of_driver")}}


def boards() -> list[dict]:
    ps = sh(["ps", "-eo", "args"])
    alive_runs = {(m[1], m[2]) for l in ps.splitlines() if ("ladder.py" in l or "closure.py" in l) and (m := re.search(r"(?:ladder|closure)\.py (\S+?)(?:\+planes)? (\S+)", l))}
    st = read_json(RW / "status.json", {})
    wanted = [b["name"] for b in st.get("boards", [])]
    wide = {r["board"]: r for r in cached("batch", 5, batch_rows)}  # the boards of the wider test (py/batch.py) are drawn in the same tree
    out = []
    for b in sorted(BOARDS.iterdir()) if BOARDS.exists() else []:
        if wanted and b.name not in wanted and b.name not in wide:
            continue
        runs = []
        for r in [d for d in sorted(b.glob("ladder-*")) + sorted(b.glob("closure-*")) + sorted(b.glob("human")) if d.is_dir() and not d.name.endswith("-prio")]:
            if r.name == "human":
                led = ledger_info(r / "ledger.json", b.name)
                if led:
                    runs.append({"run": "human", "stages": [], "ledger": led, "alive": False, "mtime": r.stat().st_mtime, "events": [], "ver": int(r.stat().st_mtime), "safety": None})
                continue
            alive = (b.name, r.name) in alive_runs
            stages = [stage_info(s, alive) for s in sorted(r.glob("stage-*"))]
            mt = max([s.stat().st_mtime for s in r.glob("stage-*/route.log")] or [r.stat().st_mtime])
            led = ledger_info(r / "ledger.json", b.name)
            events = []
            try:
                s0 = next(iter(sorted(r.glob("stage-*/relaxed.kicad_pcb"))), None)
                if s0:
                    events.append({"t": s0.stat().st_mtime, "frac": 1.0, "src": "start"})
            except OSError:
                pass
            for k, s in enumerate(stages):
                if led and k == len(stages) - 1:
                    continue  # KiCad's verdict (below) replaces the router's own count of a judged run's last stage
                if s["state"] == "done" and s["total"]:
                    events.append({"t": (r / s["stage"] / "route.log").stat().st_mtime, "frac": (s["total"] - s["routed"]) / s["total"], "src": "router", "open": s["total"] - s["routed"], "total": s["total"]})
            saf = safety_info(b.name, r.name)
            if led and led["baseline_unconnected"] and (r / "ledger.json").exists():
                # a board counts as routed only once its relaxations have been simulated and not failed: the time of the closing event is then the simulation's
                events.append({"t": max((r / "ledger.json").stat().st_mtime, saf["t"] if saf else 0), "frac": led["unconnected"] / led["baseline_unconnected"], "src": "kicad", "open": led["unconnected"],
                               "total": led["baseline_unconnected"], "safe": saf["verdict"] if saf and saf["status"] == "done" else None})
            ver = max([f.stat().st_mtime for f in r.glob("stage-*/routed_relaxed.kicad_pcb")] + [(r / "ledger.json").stat().st_mtime if (r / "ledger.json").exists() else 0, saf["t"] if saf else 0])
            view = None
            if alive:  # the router's live viewer (closure.py --view-port): the URL, with its token, is in the log of the stage that is running
                logs = sorted(r.glob("*/route.log"), key=lambda q: q.stat().st_mtime, reverse=True)[:3]
                for lg in logs:
                    try:
                        mv = re.search(r"^live view: (\S+)", lg.read_text(errors="replace"), re.M)
                    except OSError:
                        mv = None
                    if mv:
                        view = mv.group(1)
                        break
            clo = read_json(r / "closure.json", None)
            closure = None
            if clo:
                rd = [x for x in clo["rounds"] if x["kind"] == "route"]
                closure = {"rounds": len(rd), "relaxed": len(clo["assign"]), "relaxed_power": sum(1 for n in clo["assign"] if n in set(clo.get("power", []))),
                           "tiers": {t: sum(1 for v in clo["assign"].values() if v == t) for t in ("T1", "T2", "T3")},
                           "moves": [{"ref": k2, "dx": v[0], "dy": v[1]} for k2, v in clo["moves"].items()],
                           "open_by_round": [x["open"] for x in rd]}
            reached = [x for x in stages if x["state"] != "waiting"]  # how far the power nets were relaxed: the last stage that has started
            ptier = (reached[-1].get("power") or "T0") if reached and reached[-1].get("label") else None
            runs.append({"run": r.name, "view": view, "closure": closure, "power_tier": ptier, "stages": stages, "ledger": led, "alive": alive, "mtime": mt, "events": events, "ver": int(ver), "safety": saf})
        if runs:
            # a board has dozens of runs: the designer's, the live ones, the best (fewest open by the loop's own count or KiCad's, then fewest violations) and the three newest
            def key(r):
                l = r.get("ledger")
                if l:
                    sv = (r.get("safety") or {}).get("verdict")
                    return (l["unconnected"] + (1000 if l.get("shorts") else 0) + (1000 if sv == "FAIL" else 0), 0 if sv == "PASS" else 1, l["added"])  # a run with shorts or a failed simulation is no candidate; PASS before WARN
                c = r.get("closure")
                return (min(c["open_by_round"]) if c and c["open_by_round"] else 10 ** 6, 1, 10 ** 6)
            cand = [r for r in runs if r["run"] != "human"]
            keep = {r["run"] for r in sorted(cand, key=key)[:1]} | {r["run"] for r in sorted(cand, key=lambda r: -r["mtime"])[:3]} | {r["run"] for r in runs if r["alive"] or r["run"] == "human"}
            runs = [r for r in runs if r["run"] in keep]
        if runs:
            meta = next((x for x in st.get("boards", []) if x["name"] == b.name), None)
            if meta is None and not wanted and b.name not in wide:
                continue
            out.append({"name": b.name, "meta": meta or {}, "runs": runs, "batch": wide.get(b.name)})
    return out


def batch_rows() -> list[dict]:
    """reports/realworld/batch/*.jsonl: one row per board of the batch runs (py/batch.py), the newest run of a board last."""
    rows: dict[str, dict] = {}
    for f in sorted((RW / "batch").glob("*.jsonl")) if (RW / "batch").exists() else []:
        try:
            for l in f.read_text().splitlines():
                r = json.loads(l)
                lj = RW / "boards" / r["board"] / r.get("best_run", r.get("run", "")) / "ledger.json"
                if lj.exists():  # a track touching copper of another net is a short, not a compromise: such a board does not count as routed
                    try:
                        r["shorts"] = sum(1 for a in json.loads(lj.read_text())["added"] if a["type"] == "shorting_items" and any(i.startswith(("Track", "Via", "Arc")) for i in a.get("items", [])))
                    except (OSError, ValueError, KeyError):
                        pass
                rows[r["board"]] = r  # a later run of a board replaces an earlier one
        except (OSError, ValueError):
            pass
    return list(rows.values())


def status() -> dict:
    return {
        "batch": cached("batch", 5, batch_rows),
        "generated": time.time(),
        "plan": read_json(RW / "status.json", {}),
        "boards": cached("boards", 2, boards),
        "load": sh(["cat", "/proc/loadavg"]).split()[:3],
        "routes": len([l for l in sh(["ps", "-eo", "args"]).splitlines() if "tracemaker route" in l and "reports/realworld" in l]),
    }


def page() -> bytes:
    html = (STATIC / "index.html").read_text()
    css = html[html.index("<style>") + 7:html.index("</style>")]
    return (STATIC / "realworld.html").read_text().replace("/*BASE_CSS*/", css).encode()


class Handler(BaseHTTPRequestHandler):
    server_version = "TraceMakerRealworld/0.1"

    def log_message(self, fmt, *args):
        pass

    def _send(self, code: int, body: bytes, ctype: str) -> None:
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.send_header("Cache-Control", "no-store")
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:  # noqa: N802
        path = self.path.split("?", 1)[0]
        if path == "/api/status":
            self._send(200, json.dumps(status()).encode(), "application/json")
        elif path.startswith("/api/safety/"):
            parts = path[len("/api/safety/"):].split("/")
            if len(parts) == 2 and all(re.fullmatch(r"[A-Za-z0-9_.+-]+", x) for x in parts) and safety_file(*parts).exists():
                self._send(200, safety_file(*parts).read_bytes(), "application/json")
            else:
                self._send(404, b"{}", "application/json")
        elif path.startswith("/render/") and path.endswith(".svg"):
            parts = path[len("/render/"):-4].split("/")
            if len(parts) == 2 and all(re.fullmatch(r"[A-Za-z0-9_.+-]+", x) for x in parts) and (BOARDS / parts[0] / parts[1]).is_dir():
                try:
                    body = cached(f"svg:{path}:{self.path}", 20, lambda: render_svg(parts[0], parts[1]))
                except Exception as e:  # a half-written board file while a stage is saving
                    body = b""
                self._send(200 if body else 404, body or b"no render", "image/svg+xml")
            else:
                self._send(404, b"not found", "text/plain")
        elif path in ("/", "/index.html"):
            self._send(200, page(), "text/html; charset=utf-8")
        else:
            self._send(404, b"not found", "text/plain")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8770)
    a = ap.parse_args()
    srv = ThreadingHTTPServer((a.host, a.port), Handler)
    print(f"real-world routing page on http://{a.host}:{a.port}/", flush=True)
    srv.serve_forever()


if __name__ == "__main__":
    main()
