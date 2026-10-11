#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Draw a routed srj board to a PNG: pads, tracks (top blue, bottom red), vias, unrouted connections (black).

  bench/render_srj.py PROBLEM.json ROUTED.kicad_pcb OUT.png [ROUTE.json]   (route.json: its `failures` name the open ones)
"""
import json, re, sys
from PIL import Image, ImageDraw

prob = json.load(open(sys.argv[1]))
txt = open(sys.argv[2]).read()
b = prob["bounds"]
S = 140  # px per mm
W, H = int((b["maxX"] - b["minX"]) * S) + 20, int((b["maxY"] - b["minY"]) * S) + 20
im = Image.new("RGB", (W, H), "white")
d = ImageDraw.Draw(im, "RGBA")
def P(x, y): return ((x - b["minX"]) * S + 10, (b["maxY"] - y) * S + 10)  # SRJ y up
for o in prob["obstacles"]:
    cx, cy = o["center"]["x"], o["center"]["y"]
    x0, y0 = P(cx - o["width"] / 2, cy + o["height"] / 2); x1, y1 = P(cx + o["width"] / 2, cy - o["height"] / 2)
    col = (0, 0, 255, 70) if "top" in o["layers"] else (255, 0, 0, 70)
    d.rectangle([x0, y0, x1, y1], fill=col, outline=(80, 80, 80, 255))
for m in re.finditer(r'\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer "(\w)\.Cu"\)', txt):
    x0, y0, x1, y1, w = map(float, m.groups()[:5])
    col = (30, 60, 255, 150) if m.group(6) == "F" else (255, 40, 40, 150)
    d.line([P(x0, -y0), P(x1, -y1)], fill=col, width=max(1, int(w * S)))
for m in re.finditer(r'\(via\s+\(at ([-\d.]+) ([-\d.]+)\)\s+\(size ([\d.]+)\)', txt):
    x, y, s = map(float, m.groups()); px, py = P(x, -y)
    d.ellipse([px - s * S / 2, py - s * S / 2, px + s * S / 2, py + s * S / 2], fill=(0, 160, 0, 200), outline="black")
if len(sys.argv) > 4:
    open_ = {f.split(":")[0] for f in json.load(open(sys.argv[4])).get("failures", [])}
    for c in prob["connections"]:
        if c["name"] in open_:
            p = c["pointsToConnect"]
            d.line([P(p[0]["x"], p[0]["y"]), P(p[-1]["x"], p[-1]["y"])], fill=(0, 0, 0, 255), width=2)
im.save(sys.argv[3])
