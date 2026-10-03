#!/usr/bin/env python3
"""Render a TraceMaker event recording (`tracemaker route --record`) as video frames.

  build/report-venv/bin/python report/render_events.py events.jsonl unrouted.kicad_pcb outdir --step 0.2667 \
      [--width 1150 --height 898]

Replays track/via add and remove events in time order and writes one PNG per time step (frame_00000.png ...),
in the report's board style: front copper red, back blue, pads gold, vias grey, airwires of still-unconnected
nets cyan. The last frame is the final routed state.
"""
import argparse
import importlib.util
import json
import math
import pathlib

ROOT = pathlib.Path(__file__).resolve().parent.parent
spec = importlib.util.spec_from_file_location("mf", ROOT / "report/make_figures.py")
mf = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mf)
plt = mf.plt
from matplotlib.collections import LineCollection, PatchCollection  # noqa: E402
from matplotlib.patches import Circle, Polygon  # noqa: E402


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("events")
    ap.add_argument("board")
    ap.add_argument("outdir")
    ap.add_argument("--step", type=float, default=0.2667, help="seconds of routing per frame")
    ap.add_argument("--width", type=int, default=1150)
    ap.add_argument("--height", type=int, default=898)
    a = ap.parse_args()
    out = pathlib.Path(a.outdir)
    out.mkdir(parents=True, exist_ok=True)
    events = [json.loads(line) for line in open(a.events)]
    events = [e for e in events if e.get("type") in ("track_add", "track_remove", "via_add", "via_remove")]
    events.sort(key=lambda e: e["t"])
    t_end = events[-1]["t"] if events else 0.0

    d = mf.board_json(pathlib.Path(a.board))
    layers = d["copper_layers"]
    col = {0: "#c83434", len(layers) - 1: "#3b6fd1"}
    dpi = 100
    fig = plt.figure(figsize=(a.width / dpi, a.height / dpi), dpi=dpi)
    ax = fig.add_axes([0, 0, 1, 1])
    ax.set_facecolor("#101418")
    fig.patch.set_facecolor("#101418")
    edges = mf.edge_cuts(pathlib.Path(a.board))
    for seg in edges:
        xs, ys = zip(*seg)
        ax.plot(xs, ys, color="#e9d66b", lw=0.8)
    ax.add_collection(PatchCollection([Polygon(mf.pad_poly(p), closed=True) for p in d["pads"]], facecolor="#b8a46a",
                                      edgecolor="none", alpha=0.9, zorder=4))
    xs = [p["x"] / 1e6 for p in d["pads"]] + [x for s in edges for x, _ in s]
    ys = [p["y"] / 1e6 for p in d["pads"]] + [y for s in edges for _, y in s]
    x0, x1, y0, y1 = min(xs) - 1, max(xs) + 1, min(ys) - 1, max(ys) + 1
    # Keep the panel's aspect: pad the shorter side.
    want = a.width / a.height
    if (x1 - x0) / (y1 - y0) < want:
        c, w = (x0 + x1) / 2, (y1 - y0) * want
        x0, x1 = c - w / 2, c + w / 2
    else:
        c, h = (y0 + y1) / 2, (x1 - x0) / want
        y0, y1 = c - h / 2, c + h / 2
    ax.set_xlim(x0, x1)
    ax.set_ylim(y1, y0)
    ax.set_axis_off()
    pt_per_mm = a.width / dpi * 72 / (x1 - x0)

    tracks, vias = {}, {}
    coll = {l: LineCollection([], colors=col.get(l, "#c9a227"), capstyle="round", zorder=2 + (l == 0)) for l in range(len(layers))}
    for c_ in coll.values():
        ax.add_collection(c_)
    via_coll = PatchCollection([], facecolor="#cfcfcf", edgecolor="none", zorder=5)
    ax.add_collection(via_coll)
    label = ax.text(0.015, 0.985, "", transform=ax.transAxes, color="white", fontsize=15, va="top", family="DejaVu Sans",
                    bbox=dict(boxstyle="round,pad=0.3", fc="#101418", ec="none", alpha=0.8), zorder=10)

    def draw(t, n):
        for l, c_ in coll.items():
            segs = [((tr["a"][0] / 1e6, tr["a"][1] / 1e6), (tr["b"][0] / 1e6, tr["b"][1] / 1e6)) for tr in tracks.values() if tr["layer"] == l]
            ws = [max(tr["w"] / 1e6 * pt_per_mm, 0.3) for tr in tracks.values() if tr["layer"] == l]
            c_.set_segments(segs)
            c_.set_linewidths(ws or [1])
        via_coll.set_paths([Circle((v["p"][0] / 1e6, v["p"][1] / 1e6), v["d"] / 2e6) for v in vias.values()])
        label.set_text(f"{len(tracks)} segments · {len(vias)} vias")
        fig.savefig(out / f"frame_{n:05d}.png", dpi=dpi, facecolor=fig.get_facecolor())

    i, n, t = 0, 0, 0.0
    while True:
        while i < len(events) and events[i]["t"] <= t:
            e = events[i]
            if e["type"] == "track_add":
                tracks[e["track"]["id"]] = e["track"]
            elif e["type"] == "track_remove":
                tracks.pop(e["id"], None)
            elif e["type"] == "via_add":
                vias[e["via"]["id"]] = e["via"]
            elif e["type"] == "via_remove":
                vias.pop(e["id"], None)
            i += 1
        draw(t, n)
        n += 1
        if t >= t_end:
            break
        t = min(t + a.step, t_end)
    print(f"{n} frames, routing time {t_end:.1f} s, final {len(tracks)} segments, {len(vias)} vias")


if __name__ == "__main__":
    main()
