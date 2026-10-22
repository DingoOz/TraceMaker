# SPDX-License-Identifier: GPL-3.0-or-later
"""Geometry for the ledger and the parasitic extraction: gaps between routed copper, and the length over which two tracks run side by side."""
from __future__ import annotations

import math

EPS = 1e-9


def seg_point(ax, ay, bx, by, px, py):
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 < EPS else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    qx, qy = ax + t * dx, ay + t * dy
    return math.hypot(px - qx, py - qy), t


def seg_seg(a, b):
    """Minimum distance between segments a=(x1,y1,x2,y2) and b; 0 if they cross."""
    ax, ay, bx, by = a
    cx, cy, dx, dy = b
    # crossing test
    def orient(px, py, qx, qy, rx, ry):
        return (qx - px) * (ry - py) - (qy - py) * (rx - px)
    o1, o2 = orient(ax, ay, bx, by, cx, cy), orient(ax, ay, bx, by, dx, dy)
    o3, o4 = orient(cx, cy, dx, dy, ax, ay), orient(cx, cy, dx, dy, bx, by)
    if o1 * o2 < 0 and o3 * o4 < 0:
        return 0.0
    return min(seg_point(ax, ay, bx, by, cx, cy)[0], seg_point(ax, ay, bx, by, dx, dy)[0],
               seg_point(cx, cy, dx, dy, ax, ay)[0], seg_point(cx, cy, dx, dy, bx, by)[0])


def parallel_run(a, b, max_gap: float):
    """Length over which segments a and b run side by side within max_gap centre-to-centre (projection overlap on the direction of a, only when the
    angle between them is under 10 degrees), and the mean centre distance there. (0, None) otherwise."""
    ax, ay, bx, by = a
    cx, cy, dx, dy = b
    la, lb = math.hypot(bx - ax, by - ay), math.hypot(dx - cx, dy - cy)
    if la < EPS or lb < EPS:
        return 0.0, None
    ux, uy = (bx - ax) / la, (by - ay) / la
    vx, vy = (dx - cx) / lb, (dy - cy) / lb
    if abs(ux * vy - uy * vx) > math.sin(math.radians(10)):
        return 0.0, None
    t0, t1 = sorted(((cx - ax) * ux + (cy - ay) * uy, (dx - ax) * ux + (dy - ay) * uy))
    lo, hi = max(0.0, t0), min(la, t1)
    if hi - lo < 1e-6:
        return 0.0, None
    # centre distance at the middle of the overlap
    mx, my = ax + ux * (lo + hi) / 2, ay + uy * (lo + hi) / 2
    d = seg_point(cx, cy, dx, dy, mx, my)[0]
    return hi - lo, d if d <= max_gap else None


def rect_corners(px, py, w, h, rot_deg):
    c, s = math.cos(math.radians(-rot_deg)), math.sin(math.radians(-rot_deg))
    pts = []
    for lx, ly in ((-w / 2, -h / 2), (w / 2, -h / 2), (w / 2, h / 2), (-w / 2, h / 2)):
        pts.append((px + lx * c - ly * s, py + lx * s + ly * c))
    return pts


def seg_pad_gap(seg, pad):
    """Edge-to-edge gap between a track centre line (minus its half width, done by the caller) and a pad outline (rectangle, rotated; round/oval as
    the bounding rectangle's inscribed shape approximated by the rectangle with corners cut by the smaller half-size)."""
    if pad.shape in ("circle",):
        r = min(pad.w, pad.h) / 2
        d = seg_point(*seg, pad.x, pad.y)[0]
        return max(0.0, d - r)
    if pad.shape in ("oval",):
        # stadium: segment between the two cap centres, radius = smaller half size
        r = min(pad.w, pad.h) / 2
        long_ = max(pad.w, pad.h) / 2 - r
        horiz = pad.w >= pad.h
        ang = math.radians(-pad.rot)
        ex, ey = (math.cos(ang), math.sin(ang)) if horiz else (-math.sin(ang), math.cos(ang))
        cap = (pad.x - ex * long_, pad.y - ey * long_, pad.x + ex * long_, pad.y + ey * long_)
        return max(0.0, seg_seg(seg, cap) - r)
    rr = 0.0
    if pad.shape == "roundrect" and pad.rratio > 0:
        rr = pad.rratio * min(pad.w, pad.h)
    if rr > 0:  # rounded rectangle: the core rectangle shrunk by the corner radius, and the radius added back
        corners = rect_corners(pad.x, pad.y, pad.w - 2 * rr, pad.h - 2 * rr, pad.rot)
        return max(0.0, _poly_gap(seg, corners) - rr)
    corners = rect_corners(pad.x, pad.y, pad.w, pad.h, pad.rot)
    return _poly_gap(seg, corners)


def _poly_gap(seg, corners):
    # inside?
    inside = True
    for i in range(4):
        x1, y1 = corners[i]
        x2, y2 = corners[(i + 1) % 4]
        for (px, py) in ((seg[0], seg[1]), (seg[2], seg[3])):
            if (x2 - x1) * (py - y1) - (y2 - y1) * (px - x1) < 0:
                inside = False
    if inside:
        return 0.0
    return min(seg_seg(seg, (corners[i][0], corners[i][1], corners[(i + 1) % 4][0], corners[(i + 1) % 4][1])) for i in range(4))
