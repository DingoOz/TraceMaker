# SPDX-License-Identifier: GPL-3.0-or-later
"""Text-level edits of a .kicad_pcb that leave everything else byte-for-byte: top-level forms by span, a footprint moved, the copper that touches a
moved part's pads removed, the board's own rule blocks put back on a routed copy."""
from __future__ import annotations

import math
import re

HEAD = re.compile(r"\(\s*([A-Za-z_][A-Za-z0-9_]*)")


def toplevel(text: str) -> list[tuple[str, int, int]]:
    """(head, start, end) of every form that is a direct child of the root form; end is exclusive."""
    out, depth, i, n = [], 0, 0, len(text)
    start = -1
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n and text[i] != '"':
                i += 2 if text[i] == "\\" else 1
        elif c == "(":
            depth += 1
            if depth == 2:
                start = i
        elif c == ")":
            if depth == 2:
                m = HEAD.match(text, start)
                out.append((m.group(1) if m else "", start, i + 1))
            depth -= 1
        i += 1
    return out


def ref_of(block: str) -> str:
    m = re.search(r'\(property "Reference" "([^"]*)"', block) or re.search(r'\(fp_text reference "?([^"\s)]+)"?', block)
    return m.group(1) if m else ""


def move_part(text: str, ref: str, dx: float, dy: float) -> str:
    """The footprint `ref` displaced by (dx, dy) mm (pads follow: they are relative to it). Unknown ref: unchanged."""
    for head, a, b in toplevel(text):
        if head in ("footprint", "module") and ref_of(text[a:b]) == ref:
            m = re.search(r"\(at ([-0-9.]+) ([-0-9.]+)((?: [-0-9.]+)?)\)", text[a:b])
            if not m:
                return text
            x, y = float(m.group(1)) + dx, float(m.group(2)) + dy
            new = f"(at {x:.4f} {y:.4f}{m.group(3)})"
            return text[:a + m.start()] + new + text[a + m.end():]
    return text


def part_xy(text: str, ref: str):
    for head, a, b in toplevel(text):
        if head in ("footprint", "module") and ref_of(text[a:b]) == ref:
            m = re.search(r"\(at ([-0-9.]+) ([-0-9.]+)", text[a:b])
            return (float(m.group(1)), float(m.group(2))) if m else None
    return None


NUM = r"(-?[0-9.]+)"


def copper(text: str):
    """[(kind, start, end, net, points)] for every segment and via; points are (x, y) tuples (segment: both ends; via: its centre)."""
    out = []
    for head, a, b in toplevel(text):
        if head not in ("segment", "via"):
            continue
        s = text[a:b]
        net = re.search(r"\(net (\d+)\)", s)
        if not net:
            continue
        if head == "segment":
            st, en = re.search(rf"\(start {NUM} {NUM}\)", s), re.search(rf"\(end {NUM} {NUM}\)", s)
            pts = [(float(st[1]), float(st[2])), (float(en[1]), float(en[2]))]
        else:
            at = re.search(rf"\(at {NUM} {NUM}\)", s)
            pts = [(float(at[1]), float(at[2]))]
        out.append((head, a, b, int(net.group(1)), pts))
    return out


def strip_touching(text: str, seeds: list[tuple[float, float, float, int]]) -> tuple[str, int]:
    """Remove the copper of a net that is connected to a seed (x, y, radius, net): the pads of a part that is about to move. Returns (text, items removed).
    Connected = sharing an end point (to 5 um) through segments and vias of the same net; a connection is then routed again."""
    items = copper(text)
    key = lambda p: (round(p[0], 3), round(p[1], 3))
    by_pt: dict[tuple, list[int]] = {}
    for k, (_, _, _, net, pts) in enumerate(items):
        for p in pts:
            by_pt.setdefault((net, *key(p)), []).append(k)
    todo, gone = [], set()
    for k, (_, _, _, net, pts) in enumerate(items):
        for (sx, sy, r, snet) in seeds:
            if net == snet and any(math.hypot(p[0] - sx, p[1] - sy) <= r for p in pts):
                todo.append(k)
                break
    while todo:
        k = todo.pop()
        if k in gone:
            continue
        gone.add(k)
        net = items[k][3]
        for p in items[k][4]:
            todo += [j for j in by_pt.get((net, *key(p)), []) if j not in gone]
    for k in sorted(gone, reverse=True):
        a, b = items[k][1], items[k][2]
        # also drop the whitespace that led the form, so the file does not grow blank lines
        j = a
        while j > 0 and text[j - 1] in " \t":
            j -= 1
        if j > 0 and text[j - 1] == "\n":
            j -= 1
        text = text[:j] + text[b:]
    return text, len(gone)


def restore_rules(routed: str, src: str) -> str:
    """`routed` with the setup block and the net classes of `src` (the board's own rules) in place of whatever it carries now."""
    ours = toplevel(routed)
    theirs = toplevel(src)
    keep = [text for h, a, b in theirs for text in [src[a:b]] if h in ("setup", "net_class")]
    out, placed = [], False
    pos = 0
    for h, a, b in ours:
        if h in ("setup", "net_class"):
            out.append(routed[pos:a])
            if not placed:
                out.append("\n  ".join(keep))
                placed = True
            else:
                while out and out[-1].strip() == "":
                    out.pop()
            pos = b
    out.append(routed[pos:])
    return "".join(out)


def zone_net(block: str) -> str:
    m = re.search(r'\(net_name ("[^"]*"|[^)\s]+)\)', block)
    return m.group(1).strip('"') if m else ""


def remove_zones(text: str, nets) -> str:
    """The board without the pours of `nets` (the router then connects those nets pad to pad with copper, and does not count on a pour that a refill may cut)."""
    nets = set(nets)
    if not nets:
        return text
    cut = [(a, b) for h, a, b in toplevel(text) if h == "zone" and zone_net(text[a:b]) in nets]
    for a, b in reversed(cut):
        j = a
        while j > 0 and text[j - 1] in " \t":
            j -= 1
        if j > 0 and text[j - 1] == "\n":
            j -= 1
        text = text[:j] + text[b:]
    return text


def add_zones(routed: str, src: str, nets) -> str:
    """`routed` plus the pours of `nets` as `src` has them: what KiCad judges is the routed copper and the designer's pours together."""
    nets = set(nets)
    blocks = [src[a:b] for h, a, b in toplevel(src) if h == "zone" and zone_net(src[a:b]) in nets]
    if not blocks:
        return routed
    end = routed.rstrip().rfind(")")
    return routed[:end].rstrip("\n") + "\n" + "\n".join("  " + z for z in blocks) + "\n)\n"


def _seg_dist(px, py, ax, ay, bx, by) -> float:
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def strip_window(text: str, centers: list[tuple[float, float]], radius: float, keep_nets=(), max_items: int = 200) -> tuple[str, int, set]:
    """Remove the routed copper that comes within `radius` of any centre: whole connected pieces of a net (so no stub is left dangling), except nets in
    `keep_nets` (net ids) and pieces larger than `max_items` (a power or ground tree is an obstacle, not something to route again). Returns (text, items
    removed, ids of the nets that lost copper)."""
    items = copper(text)
    key = lambda p: (round(p[0], 3), round(p[1], 3))
    by_pt: dict[tuple, list[int]] = {}
    for k, (_, _, _, net, pts) in enumerate(items):
        for p in pts:
            by_pt.setdefault((net, *key(p)), []).append(k)
    near = []
    for k, (kind, _, _, net, pts) in enumerate(items):
        if net in keep_nets:
            continue
        for (cx, cy) in centers:
            d = _seg_dist(cx, cy, pts[0][0], pts[0][1], pts[-1][0], pts[-1][1]) if len(pts) > 1 else math.hypot(cx - pts[0][0], cy - pts[0][1])
            if d <= radius:
                near.append(k)
                break
    gone: set[int] = set()
    lost: set[int] = set()
    seen: set[int] = set()
    for k0 in near:
        if k0 in seen:
            continue
        comp, todo = set(), [k0]
        while todo:
            k = todo.pop()
            if k in comp:
                continue
            comp.add(k)
            net = items[k][3]
            for p in items[k][4]:
                todo += [j for j in by_pt.get((net, *key(p)), []) if j not in comp]
        seen |= comp
        if len(comp) <= max_items:
            gone |= comp
            lost |= {items[k][3] for k in comp}
    for k in sorted(gone, reverse=True):
        a, b = items[k][1], items[k][2]
        j = a
        while j > 0 and text[j - 1] in " \t":
            j -= 1
        if j > 0 and text[j - 1] == "\n":
            j -= 1
        text = text[:j] + text[b:]
    return text, len(gone), lost


def merge_copper(base: str, routed: str) -> str:
    """`base` (a board file with its rules) plus every segment and via of `routed`, as copper that is already on the board (fixed for the router)."""
    forms = [routed[a:b] for h, a, b in toplevel(routed) if h in ("segment", "via")]
    if not forms:
        return base
    end = base.rstrip().rfind(")")
    return base[:end].rstrip("\n") + "\n" + "\n".join("  " + f for f in forms) + "\n)\n"


def strip_nets(text: str, net_ids) -> tuple[str, int]:
    """The board without any track or via of the nets `net_ids` (whole nets, wherever they run): a group of nets that is to be routed again from its pads."""
    ids = set(net_ids)
    items = [(a, b) for kind, a, b, net, _ in copper(text) if net in ids]
    for a, b in reversed(items):
        j = a
        while j > 0 and text[j - 1] in " \t":
            j -= 1
        if j > 0 and text[j - 1] == "\n":
            j -= 1
        text = text[:j] + text[b:]
    return text, len(items)
