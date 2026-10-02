#!/usr/bin/env python3
"""Side-by-side figure of one board routed by the human designer, TraceMaker and Freerouting (bench/compare.py).

  build/report-venv/bin/python report/compare_figure.py AmpOne_dev-AmpOne

Writes report/figures/compare_<board>.pdf (whole boards) and compare_<board>_zoom.pdf (the same window, centred
on the densest pad area, in every panel), plus a LaTeX table fragment report/figures/compare_<board>.tex.
"""
import importlib.util
import json
import os
import pathlib
import sys

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("mf", ROOT / "report/make_figures.py")
mf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mf)
plt = mf.plt


def densest_window(d, size):
    pts = [(p["x"] / 1e6, p["y"] / 1e6) for p in d["pads"]]
    best, where = -1, pts[0]
    for cx, cy in pts:
        n = sum(1 for x, y in pts if abs(x - cx) <= size / 2 and abs(y - cy) <= size / 2)
        if n > best:
            best, where = n, (cx, cy)
    cx, cy = where
    return (cx - size / 2, cy - size / 2, cx + size / 2, cy + size / 2)


def main():
    board = sys.argv[1]
    zoom_mm = float(sys.argv[2]) if len(sys.argv) > 2 else 22
    q = json.loads((ROOT / "build/compare" / board / "quality.json").read_text())
    runs = [r for r in q["runs"] if r.get("file")]
    n = len(runs)

    def title(r):
        if r["label"] == "Human original":
            return "Human original"
        state = "clean" if r.get("clean") else (f"{sum(r['router_errors'].values())} DRC errors" if r.get("router_errors") else
                                                f"{r.get('unconnected')} unrouted")
        return f"{r['label'].replace('Freerouting', 'FR')} · {state}"

    if n == 4:  # 2 x 2 keeps each board large enough to read
        fig, axes = plt.subplots(2, 2, figsize=(6.4, 6.5))
        axes = axes.ravel()
    else:
        fig, axes = plt.subplots(1, n, figsize=(2.5 * n, 3.1))
    for ax, r in zip(axes, runs):
        mf.draw_board(ax, pathlib.Path(r["file"]))
        ax.set_title(title(r), fontsize=8.5)
    fig.subplots_adjust(wspace=0.04, hspace=0.12)
    mf.save(fig, f"compare_{board}.pdf")

    window = densest_window(mf.board_json(pathlib.Path(runs[0]["file"])), zoom_mm)
    fig, axes = plt.subplots(1, n, figsize=(2.3 * n, 2.4))
    for ax, r in zip(axes, runs):
        mf.draw_board(ax, pathlib.Path(r["file"]), crop=window)
        ax.set_title(r["label"], fontsize=7.5)
    fig.subplots_adjust(wspace=0.04)
    mf.save(fig, f"compare_{board}_zoom.pdf")

    # LaTeX table fragment with the quality metrics.
    def fmt(v, f="{:,.0f}"):
        return "--" if v is None else f.format(v)

    lines = ["\\begin{tabular}{l" + "r" * n + "}", "\\toprule", " & " + " & ".join(r["label"].replace("Freerouting", "FR") for r in runs) + " \\\\",
             "\\midrule"]
    rows = [
        ("Connections routed", lambda r: fmt(100 * r["completion"], "{:.1f}\\%") if r.get("completion") is not None else "--"),
        ("Router-introduced DRC errors", lambda r: "n/a" if r["label"] == "Human original" else str(sum(r.get("router_errors", {}).values()))),
        ("Track length (mm)", lambda r: fmt(r.get("length_mm"))),
        ("Length / pad MST", lambda r: fmt(r.get("detour"), "{:.2f}")),
        ("Vias", lambda r: fmt(r.get("vias"))),
        ("Bends", lambda r: fmt(r.get("bends"))),
        ("Acute angles outside pads", lambda r: fmt(r.get("sharp_bends"))),
        ("Acute angles at pad centres", lambda r: fmt(r.get("sharp_at_pads"))),
        ("Narrowed track (mm)", lambda r: fmt(r.get("narrowed_mm"), "{:.1f}")),
        ("Wall time (s)", lambda r: fmt(r.get("wall_s"), "{:.0f}")),
    ]
    for name, f in rows:
        lines.append(name + " & " + " & ".join(f(r) for r in runs) + " \\\\")
    lines += ["\\bottomrule", "\\end{tabular}"]
    (mf.OUT / f"compare_{board}.tex").write_text("\n".join(lines) + "\n")
    print("wrote", f"compare_{board}.tex")


if __name__ == "__main__":
    main()
