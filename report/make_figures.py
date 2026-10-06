#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Figures for the TraceMaker LaTeX report (report/report.tex).

Reads benchmark results from bench/results/, placement results from build/place-auto/, and routed boards via
`tracemaker inspect --json`. Writes vector PDFs to report/figures/.

  build/report-venv/bin/python report/make_figures.py
"""
import glob
import json
import math
import os
import pathlib
import re
import statistics
import subprocess

import matplotlib

matplotlib.use("pdf")
import matplotlib.pyplot as plt  # noqa: E402
from matplotlib.collections import LineCollection, PatchCollection  # noqa: E402
from matplotlib.patches import Circle, Polygon  # noqa: E402

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "report/figures"
OUT.mkdir(parents=True, exist_ok=True)
RES = ROOT / "bench/results"
FIX = ROOT / "bench/data/freerouting/scripts/benchmark/fixtures/PCBench"
TM = ROOT / "build/release/src/app/tracemaker"
FINAL = "final11"
TIME_RUN = "final12-tierA"  # tier A routed on its own: final11 ran all four tiers at once, which distorts times

# Reference categorical palette (dataviz skill, light mode), slots 1-3 validate all-pairs.
C_TM, C_FR12, C_FR241 = "#2a78d6", "#eb6834", "#1baf7a"
INK, INK2, GRID = "#0b0b0b", "#52514e", "#e4e3df"

plt.rcParams.update({
    "font.size": 8.5, "axes.titlesize": 9, "axes.labelsize": 8.5, "xtick.labelsize": 8, "ytick.labelsize": 8,
    "legend.fontsize": 7.5, "axes.edgecolor": INK2, "axes.labelcolor": INK, "xtick.color": INK2, "ytick.color": INK2,
    "text.color": INK, "axes.spines.top": False, "axes.spines.right": False, "axes.grid": True, "grid.color": GRID,
    "grid.linewidth": 0.6, "axes.axisbelow": True, "pdf.fonttype": 42, "figure.dpi": 150,
})


def summary(run):
    return json.loads((RES / run / "summary.json").read_text())


def boards(run):
    return [json.loads(line) for line in (RES / run / "boards.jsonl").read_text().splitlines() if line.strip()]


def save(fig, name):
    fig.savefig(OUT / name, bbox_inches="tight", pad_inches=0.02)
    if os.environ.get("PREVIEW"):  # PNG copies for a quick look
        prev = ROOT / "build/report-preview"
        prev.mkdir(parents=True, exist_ok=True)
        fig.savefig(prev / name.replace(".pdf", ".png"), bbox_inches="tight", pad_inches=0.02, dpi=170)
    plt.close(fig)
    print("wrote", name)


# --------------------------------------------------------------------------------------------------------------
def fig_tiers():
    tiers = ["A", "B", "C", "D"]
    s = [summary(f"{FINAL}-tier{t}") for t in tiers]
    fig, ax = plt.subplots(figsize=(4.6, 2.5))
    w = 0.26
    series = [("TraceMaker", "clean_pass", C_TM), ("Freerouting 2.5.0-RC12", "fr_rc12_clean_pass", C_FR12),
              ("Freerouting 2.4.1", "fr_241_clean_pass", C_FR241)]
    for k, (lab, key, col) in enumerate(series):
        xs = [i + (k - 1) * (w + 0.02) for i in range(len(tiers))]
        vals = [100 * d[key] for d in s]
        ax.bar(xs, vals, w, color=col, label=lab, edgecolor="white", linewidth=0.8)
        for x, v in zip(xs, vals):
            ax.text(x, v + 1.5, f"{v:.0f}", ha="center", va="bottom", fontsize=6.5, color=INK2)
    ax.set_xticks(range(len(tiers)), [f"Tier {t}\n({d['boards']} boards)" for t, d in zip(tiers, s)])
    ax.set_ylabel("Clean pass (%)")
    ax.set_ylim(0, 112)
    ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, ncol=3, loc="upper center", bbox_to_anchor=(0.5, 1.16))
    save(fig, "tiers.pdf")


def fig_head_to_head():
    tiers = ["A", "B", "C", "D"]
    fig, ax = plt.subplots(figsize=(4.6, 2.0))
    cats = [("Both clean", "#8fb8ea"), ("Only TraceMaker", C_TM), ("Only Freerouting", C_FR12), ("Neither", "#d6d4ce")]
    rows = []
    for t in tiers:
        b = boards(f"{FINAL}-tier{t}")
        both = sum(1 for r in b if r.get("clean") and r.get("fr_rc12", {}).get("clean"))
        tm = sum(1 for r in b if r.get("clean") and not r.get("fr_rc12", {}).get("clean"))
        fr = sum(1 for r in b if not r.get("clean") and r.get("fr_rc12", {}).get("clean"))
        rows.append([both, tm, fr, len(b) - both - tm - fr])
    for i, r in enumerate(rows):
        left = 0
        for (lab, col), v in zip(cats, r):
            ax.barh(i, v, left=left, color=col, edgecolor="white", linewidth=1.0, label=lab if i == 0 else None, height=0.62)
            if v:
                ax.text(left + v / 2, i, str(v), ha="center", va="center", fontsize=7,
                        color="white" if col in (C_TM, C_FR12) else INK)
            left += v
    ax.set_yticks(range(len(tiers)), [f"Tier {t}" for t in tiers])
    ax.invert_yaxis()
    ax.set_xlabel("Boards (TraceMaker vs Freerouting 2.5.0-RC12)")
    ax.grid(axis="y", visible=False)
    ax.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 1.22), handlelength=1.2, columnspacing=1.0)
    save(fig, "head_to_head.pdf")


def fig_progress():
    runs = []
    for f in glob.glob(str(RES / "*/summary.json")):
        d = json.loads(pathlib.Path(f).read_text())
        # Runs given an explicit board list (held-out and dense-package sets) are recorded under the default tier
        # label: keep only runs named after the tier they sample.
        run = pathlib.Path(f).parent.name
        tier = d.get("set", "")[-1:]
        if d.get("boards", 0) >= 20 and d.get("set", "").startswith("PCBench tier") and tier in "ABCD" and re.search(rf"tier{tier}|[_-]{tier}\d*$", run):
            runs.append((os.path.getmtime(f), d))
    runs.sort(key=lambda x: x[0])
    t0 = runs[0][0]
    fig, ax = plt.subplots(figsize=(4.6, 2.6))
    cols = {"A": "#2a78d6", "B": "#eb6834", "C": "#1baf7a", "D": "#4a3aa7"}
    for tier, col in cols.items():
        pts = [((m - t0) / 3600, 100 * d["clean_pass"]) for m, d in runs if d["set"].endswith(tier)]
        if not pts:
            continue
        xs, ys = zip(*pts)
        ax.plot(xs, ys, "-o", color=col, lw=1.6, ms=3.5, label=f"Tier {tier}")
        fr = next(d["fr_rc12_clean_pass"] for m, d in runs if d["set"].endswith(tier))
        ax.axhline(100 * fr, color=col, lw=0.9, ls=(0, (3, 2)), alpha=0.8)
        ax.text(max(xs) + 0.08, ys[-1], f"{ys[-1]:.0f}%", va="center", fontsize=7, color=col)
    ax.set_xlabel("Hours since first full benchmark run")
    ax.set_ylabel("Clean pass (%)")
    ax.set_ylim(0, 105)
    ax.set_xlim(right=ax.get_xlim()[1] + 0.5)
    ax.legend(frameon=False, ncol=4, loc="upper center", bbox_to_anchor=(0.5, 1.15))
    ax.text(0.0, 2, "dashed: Freerouting 2.5.0-RC12 on the same boards", fontsize=6.5, color=INK2)
    save(fig, "progress.pdf")


def fig_completion():
    tiers = ["B", "C", "D"]
    fig, ax = plt.subplots(figsize=(4.6, 2.2))
    for i, t in enumerate(tiers):
        b = boards(f"{FINAL}-tier{t}")
        vals = sorted(100 * r["completion"] for r in b if r.get("completion") is not None)
        n = len(vals)
        for k, v in enumerate(vals):
            jitter = (k / max(n - 1, 1) - 0.5) * 0.5
            clean = v >= 99.999
            ax.plot(i + jitter, v, "o", ms=3.4, color=C_TM if clean else "#9a9893", alpha=0.9, mec="white", mew=0.4)
        mean = sum(vals) / n
        ax.plot([i - 0.32, i + 0.32], [mean, mean], color=INK, lw=1.2)
        ax.text(i - 0.05, mean - 1.2, f"mean {mean:.1f}%", ha="center", va="top", fontsize=6.5, color=INK2,
                bbox=dict(boxstyle="round,pad=0.15", fc="white", ec="none", alpha=0.85))
    ax.set_xticks(range(len(tiers)), [f"Tier {t}" for t in tiers])
    ax.set_ylabel("Connections routed (%)")
    ax.set_ylim(60, 101.5)
    ax.set_xlim(-0.5, len(tiers) - 0.2)
    ax.grid(axis="x", visible=False)
    save(fig, "completion.pdf")


def fig_time():
    b = boards(TIME_RUN)
    pts = [(r["fr_rc12"]["seconds"], r["seconds"]) for r in b if r.get("fr_rc12", {}).get("seconds") and r.get("seconds")]
    fig, ax = plt.subplots(figsize=(3.0, 2.6))
    xs, ys = zip(*pts)
    ax.scatter(xs, ys, s=12, color=C_TM, edgecolor="white", linewidth=0.4, zorder=3)
    lo, hi = 0.1, 1000
    ax.plot([lo, hi], [lo, hi], color=INK2, lw=0.8, ls="--")
    ax.text(20, 40, "equal time", rotation=45, fontsize=6.5, color=INK2, ha="center")
    ax.set_xscale("log")
    ax.set_yscale("log")
    ax.set_xlim(lo, hi)
    ax.set_ylim(lo, hi)
    ax.set_xlabel("Freerouting 2.5.0-RC12, published (s)")
    ax.set_ylabel("TraceMaker routing time (s)")
    faster = sum(1 for x, y in pts if y < x)
    ax.set_title(f"Tier A: faster on {faster} of {len(pts)} boards", fontsize=8)
    save(fig, "time.pdf")


def fig_placement():
    p = ROOT / "build/place-auto-fixed/summary.json"
    rows = [r for r in json.loads(p.read_text()) if "error" not in r]
    rows.sort(key=lambda r: r["hpwl_kept"] / r["hpwl_input"])
    fig, ax = plt.subplots(figsize=(5.0, 3.6))
    names = [r["board"].replace("kitspace_", "")[:24] for r in rows]
    ratio = [100 * (1 - r["hpwl_kept"] / r["hpwl_input"]) for r in rows]
    cols = [C_TM if r["mode"] != "input" else "#c9c7c0" for r in rows]
    ax.barh(range(len(rows)), ratio, color=cols, height=0.7, edgecolor="white", linewidth=0.5)
    for i, r in enumerate(rows):
        tag = r["mode"] if r["mode"] != "input" else "kept input"
        d = r["unrouted_kept"] - r["unrouted_input"]
        extra = f", unrouted {r['unrouted_input']}→{r['unrouted_kept']}" if d else ""
        ax.text(ratio[i] + 0.8, i, f"{tag}{extra}", va="center", fontsize=6.5, color=INK2)
    ax.set_yticks(range(len(rows)), names, fontsize=6.8)
    ax.invert_yaxis()
    ax.set_xlabel("Wirelength (HPWL) reduction vs. the human placement (%)")
    ax.set_xlim(0, 75)
    ax.grid(axis="y", visible=False)
    save(fig, "placement.pdf")


# --------------------------------------------------------------------------------------------------------------
# Board rendering
NUM = r"(-?[\d.]+)"


def edge_cuts(path):
    """Board-level Edge.Cuts lines/arcs/rects/circles as polylines (mm)."""
    s = pathlib.Path(path).read_text(errors="replace")
    segs = []
    for m in re.finditer(r"\((gr_line|gr_arc|gr_rect|gr_circle|gr_poly)\b", s):
        i, depth, j = m.start(), 0, m.start()
        while True:
            c = s[j]
            if c == "(":
                depth += 1
            elif c == ")":
                depth -= 1
                if depth == 0:
                    break
            j += 1
        blk = s[i:j + 1]
        if "Edge.Cuts" not in blk:
            continue
        kind = m.group(1)
        g = lambda k: re.search(r"\(" + k + r"\s+" + NUM + r"\s+" + NUM + r"\)", blk)  # noqa: E731
        if kind == "gr_line":
            a, b = g("start"), g("end")
            segs.append([(float(a[1]), float(a[2])), (float(b[1]), float(b[2]))])
        elif kind == "gr_rect":
            a, b = g("start"), g("end")
            x0, y0, x1, y1 = float(a[1]), float(a[2]), float(b[1]), float(b[2])
            segs.append([(x0, y0), (x1, y0), (x1, y1), (x0, y1), (x0, y0)])
        elif kind == "gr_circle":
            c, e = g("center") or g("start"), g("end")
            cx, cy = float(c[1]), float(c[2])
            r = math.hypot(float(e[1]) - cx, float(e[2]) - cy)
            segs.append([(cx + r * math.cos(t / 32 * 2 * math.pi), cy + r * math.sin(t / 32 * 2 * math.pi)) for t in range(33)])
        elif kind == "gr_arc":
            mid = g("mid")
            if mid:  # KiCad 6+: start, mid, end
                pts = [g("start"), mid, g("end")]
                (x1, y1), (x2, y2), (x3, y3) = [(float(p[1]), float(p[2])) for p in pts]
                d = 2 * (x1 * (y2 - y3) + x2 * (y3 - y1) + x3 * (y1 - y2))
                if abs(d) < 1e-12:
                    segs.append([(x1, y1), (x3, y3)])
                    continue
                ux = ((x1**2 + y1**2) * (y2 - y3) + (x2**2 + y2**2) * (y3 - y1) + (x3**2 + y3**2) * (y1 - y2)) / d
                uy = ((x1**2 + y1**2) * (x3 - x2) + (x2**2 + y2**2) * (x1 - x3) + (x3**2 + y3**2) * (x2 - x1)) / d
                r = math.hypot(x1 - ux, y1 - uy)
                a1, a2, a3 = (math.atan2(y - uy, x - ux) for x, y in ((x1, y1), (x2, y2), (x3, y3)))
                sweep = (a3 - a1) % (2 * math.pi)
                if not ((a2 - a1) % (2 * math.pi) < sweep):
                    sweep -= 2 * math.pi
                segs.append([(ux + r * math.cos(a1 + sweep * t / 16), uy + r * math.sin(a1 + sweep * t / 16)) for t in range(17)])
            else:  # KiCad 5: start = centre, end = start point, angle in degrees
                c, e, a = g("start"), g("end"), re.search(r"\(angle\s+" + NUM + r"\)", blk)
                cx, cy, ex, ey = float(c[1]), float(c[2]), float(e[1]), float(e[2])
                r, a0 = math.hypot(ex - cx, ey - cy), math.atan2(ey - cy, ex - cx)
                sw = math.radians(float(a[1])) if a else 0
                segs.append([(cx + r * math.cos(a0 + sw * t / 16), cy + r * math.sin(a0 + sw * t / 16)) for t in range(17)])
        elif kind == "gr_poly":
            pts = [(float(x), float(y)) for x, y in re.findall(r"\(xy\s+" + NUM + r"\s+" + NUM + r"\)", blk)]
            if pts:
                segs.append(pts + [pts[0]])
    return segs


def board_json(path):
    out = ROOT / "build/scratch" / (pathlib.Path(path).stem.replace(" ", "_") + ".report.json")
    out.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run([str(TM), "inspect", str(path), "--json", str(out)], check=True, capture_output=True)
    return json.loads(out.read_text())


def pad_poly(p):
    w, h, a = p["w"] / 2e6, p["h"] / 2e6, math.radians(-p.get("angle", 0.0))
    ca, sa = math.cos(a), math.sin(a)
    x, y = p["x"] / 1e6, p["y"] / 1e6
    return [(x + dx * ca - dy * sa, y + dx * sa + dy * ca) for dx, dy in ((-w, -h), (w, -h), (w, h), (-w, h))]


def airwires(d):
    """Minimum spanning tree over each net's pad centres (mm), as a stand-in for KiCad's ratsnest."""
    by_net = {}
    for p in d["pads"]:
        if p.get("net"):
            by_net.setdefault(p["net"], []).append((p["x"] / 1e6, p["y"] / 1e6))
    lines = []
    for pts in by_net.values():
        pts = list(dict.fromkeys(pts))
        if len(pts) < 2 or len(pts) > 400:
            continue
        inn, best, frm = [False] * len(pts), [math.inf] * len(pts), [0] * len(pts)
        best[0] = 0
        for _ in range(len(pts)):
            u = min((i for i in range(len(pts)) if not inn[i]), key=lambda i: best[i])
            inn[u] = True
            if u:
                lines.append([pts[frm[u]], pts[u]])
            for v in range(len(pts)):
                if not inn[v]:
                    dd = math.dist(pts[u], pts[v])
                    if dd < best[v]:
                        best[v], frm[v] = dd, u
    return lines


def draw_board(ax, path, ratsnest=False, crop=None, lw_scale=1.0):
    d = board_json(path)
    layers = d["copper_layers"]
    lay_col = {layers[0]: "#c83434", layers[-1]: "#3b6fd1"}
    inner = ["#c9a227", "#2f9e6a", "#a24bc9", "#d1803b"]
    for i, l in enumerate(layers[1:-1]):
        lay_col[l] = inner[i % len(inner)]
    ax.set_facecolor("#101418")
    for seg in edge_cuts(path):
        xs, ys = zip(*seg)
        ax.plot(xs, ys, color="#e9d66b", lw=0.6 * lw_scale, zorder=1)
    # Tracks: back layer first so the top layer reads on top.
    for l in reversed(layers):
        segs = [[(t["sx"] / 1e6, t["sy"] / 1e6), (t["ex"] / 1e6, t["ey"] / 1e6)] for t in d["tracks"] if t["layer"] == l]
        widths = [t["width"] / 1e6 for t in d["tracks"] if t["layer"] == l]
        if segs:
            # Line widths in points from mm: scaled after the axes limits are known (see below).
            lc = LineCollection(segs, colors=lay_col[l], alpha=0.85, zorder=2 + (layers.index(l) == 0), capstyle="round")
            lc._tm_mm = widths  # noqa: SLF001
            ax.add_collection(lc)
    pads = [Polygon(pad_poly(p), closed=True) for p in d["pads"]]
    ax.add_collection(PatchCollection(pads, facecolor="#b8a46a", edgecolor="none", alpha=0.9, zorder=4))
    vias = [Circle((v["x"] / 1e6, v["y"] / 1e6), v["size"] / 2e6) for v in d["vias"]]
    ax.add_collection(PatchCollection(vias, facecolor="#cfcfcf", edgecolor="none", zorder=5))
    if ratsnest:
        ax.add_collection(LineCollection(airwires(d), colors="#7fd3ff", linewidths=0.35 * lw_scale, alpha=0.8, zorder=6))
    xs = [p["x"] / 1e6 for p in d["pads"]] + [x for s in edge_cuts(path) for x, _ in s]
    ys = [p["y"] / 1e6 for p in d["pads"]] + [y for s in edge_cuts(path) for _, y in s]
    if crop:
        x0, y0, x1, y1 = crop
    else:
        x0, x1, y0, y1 = min(xs) - 1, max(xs) + 1, min(ys) - 1, max(ys) + 1
    ax.set_xlim(x0, x1)
    ax.set_ylim(y1, y0)
    ax.set_aspect("equal")
    ax.set_xticks([])
    ax.set_yticks([])
    ax.grid(False)
    for sp in ax.spines.values():
        sp.set_visible(False)
    # Convert track widths (mm) to points for this axes.
    fig = ax.figure
    fig.canvas.draw()
    bb = ax.get_window_extent()
    pt_per_mm = bb.width / fig.dpi * 72 / (x1 - x0)
    for coll in ax.collections:
        if hasattr(coll, "_tm_mm"):
            coll.set_linewidths([max(w * pt_per_mm, 0.15) for w in coll._tm_mm])
    return d


def fig_board(name, tier, fname, w=4.6, h=3.4, crop=None):
    path = RES / f"{FINAL}-tier{tier}" / "boards" / f"{name}.kicad_pcb"
    fig, ax = plt.subplots(figsize=(w, h))
    d = draw_board(ax, path, crop=crop)
    save(fig, fname)
    return d


def fig_before_after(name, tier):
    routed = RES / f"{FINAL}-tier{tier}" / "boards" / f"{name}.kicad_pcb"
    unrouted = FIX / name / "unrouted.kicad_pcb"
    fig, axes = plt.subplots(1, 2, figsize=(6.4, 2.9))
    draw_board(axes[0], unrouted, ratsnest=True)
    axes[0].set_title("Input: pads and airwires", fontsize=8)
    d = draw_board(axes[1], routed)
    axes[1].set_title(f"Routed: {len(d['tracks'])} segments, {len(d['vias'])} vias", fontsize=8)
    fig.subplots_adjust(wspace=0.04)
    save(fig, "before_after.pdf")


def main():
    if os.environ.get("QUALITY_ONLY"):
        fig_quality_clean()
        fig_quality_ratios()
        fig_quality_time()
        return
    fig_tiers()
    fig_head_to_head()
    fig_progress()
    fig_completion()
    fig_time()
    fig_placement()
    fig_before_after("motor-3xdrv8833-hw_ver1", "B")
    fig_board("Hangul-Clock_Hangul", "A", "board_a.pdf", 3.1, 2.6)
    fig_board("kitspace_40-channel-hv-switching-board", "C", "board_c.pdf", 3.1, 3.4)
    fig_board("EUC-VESC_electronics_sept", "D", "board_d.pdf", 3.1, 2.6)
    fig_board("draco_draco", "C", "board_draco.pdf", 3.1, 2.6)
    # Close-up: fine-pitch escapes around the motor board's QFN.
    d = board_json(RES / f"{FINAL}-tierB/boards/motor-3xdrv8833-hw_ver1.kicad_pcb")
    fps = {f["ref"]: (f["x"] / 1e6, f["y"] / 1e6) for f in d["footprints"]}
    cx, cy = fps.get("U2", (None, None))
    if cx is not None:
        fig_board("motor-3xdrv8833-hw_ver1", "B", "closeup.pdf", 3.1, 3.1, crop=(cx - 7, cy - 7, cx + 7, cy + 7))



# --------------------------------------------------------------------------------------------------------------
# Held-out quality benchmark (bench/quality_bench.py)
QRUN = os.environ.get("QUALITY_RUN", "quality-1")
QLABELS = [("TraceMaker", C_TM), ("Freerouting 2.5.0-RC12", C_FR12), ("Freerouting 1.9.0", "#4a3aa7")]


def quality_rows():
    p = RES / QRUN / "quality.jsonl"
    rows = [json.loads(line) for line in p.read_text().splitlines() if line.strip()]
    return [r for r in rows if "runs" in r]


def run_of(row, label):
    return next((x for x in row["runs"] if x["label"] == label), None)


def fig_quality_clean():
    rows = quality_rows()
    tiers = sorted({r["tier"] for r in rows})
    fig, ax = plt.subplots(figsize=(4.6, 2.5))
    w = 0.26
    for k, (lab, col) in enumerate(QLABELS):
        xs, vals = [], []
        for i, t in enumerate(tiers):
            rs = [run_of(r, lab) for r in rows if r["tier"] == t]
            rs = [x for x in rs if x and x.get("completion") is not None]
            if not rs:
                continue
            xs.append(i + (k - 1) * (w + 0.02))
            vals.append(100 * sum(1 for x in rs if x.get("clean")) / len(rs))
        ax.bar(xs, vals, w, color=col, label=lab.replace("Freerouting", "FR"), edgecolor="white", linewidth=0.8)
        for x, v in zip(xs, vals):
            ax.text(x, v + 1.5, f"{v:.0f}", ha="center", va="bottom", fontsize=6.5, color=INK2)
    n = {t: sum(1 for r in rows if r["tier"] == t) for t in tiers}
    ax.set_xticks(range(len(tiers)), [f"Tier {t}\n({n[t]} boards)" for t in tiers])
    ax.set_ylabel("Clean in KiCad's DRC (%)")
    ax.set_ylim(0, 112)
    ax.grid(axis="x", visible=False)
    ax.legend(frameon=False, ncol=3, loc="upper center", bbox_to_anchor=(0.5, 1.16))
    save(fig, "quality_clean.pdf")


def fig_quality_ratios():
    rows = quality_rows()
    metrics = [("length_mm", "Track length"), ("vias", "Vias"), ("bends", "Bends")]
    others = [lab for lab, _ in QLABELS[1:]]
    fig, axes = plt.subplots(1, len(others), figsize=(6.4, 2.4), sharey=True)
    for ax, other in zip(axes, others):
        data = []
        for key, _ in metrics:
            v = []
            for r in rows:
                a, b = run_of(r, "TraceMaker"), run_of(r, other)
                if a and b and a.get("completion") == 1.0 and b.get("completion") == 1.0 and b.get(key):
                    v.append(a[key] / b[key])
            data.append(v)
        for i, v in enumerate(data):
            if not v:
                continue
            v = sorted(v)
            jit = [(j / max(len(v) - 1, 1) - 0.5) * 0.45 for j in range(len(v))]
            ax.scatter([i + x for x in jit], v, s=9, color=C_TM, alpha=0.75, edgecolor="white", linewidth=0.3, zorder=3)
            med = statistics.median(v)
            ax.plot([i - 0.3, i + 0.3], [med, med], color=INK, lw=1.3, zorder=4)
            ax.text(i + 0.33, med, f"{med:.2f}", va="center", fontsize=6.5, color=INK)
        ax.axhline(1.0, color=INK2, lw=0.8, ls="--")
        ax.set_xticks(range(len(metrics)), [m[1] for m in metrics])
        ax.set_yscale("log")
        ax.set_title(f"TraceMaker / {other.replace('Freerouting', 'FR')}", fontsize=8)
        ax.grid(axis="x", visible=False)
        n = len(data[0])
        ax.text(0.02, 0.02, f"{n} boards both complete", transform=ax.transAxes, fontsize=6.5, color=INK2)
    axes[0].set_ylabel("Ratio (below 1: TraceMaker less)")
    save(fig, "quality_ratios.pdf")


def fig_quality_time():
    rows = quality_rows()
    fig, ax = plt.subplots(figsize=(4.6, 2.3))
    for i, (lab, col) in enumerate(QLABELS):
        v = sorted(x["wall_s"] for r in rows for x in [run_of(r, lab)] if x and x.get("wall_s"))
        if not v:
            continue
        jit = [(j / max(len(v) - 1, 1) - 0.5) * 0.5 for j in range(len(v))]
        ax.scatter([i + x for x in jit], v, s=9, color=col, alpha=0.8, edgecolor="white", linewidth=0.3)
        med = statistics.median(v)
        ax.plot([i - 0.32, i + 0.32], [med, med], color=INK, lw=1.3)
        ax.text(i + 0.35, med, f"median {med:.0f} s", va="center", fontsize=6.5, color=INK2)
    ax.set_yscale("log")
    ax.set_xticks(range(len(QLABELS)), [l.replace("Freerouting", "FR") for l, _ in QLABELS])
    ax.set_ylabel("Wall time per board (s)")
    ax.set_xlim(-0.5, len(QLABELS) - 0.1)
    ax.grid(axis="x", visible=False)
    save(fig, "quality_time.pdf")


if __name__ == "__main__":
    main()
