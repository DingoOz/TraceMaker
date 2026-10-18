# SPDX-License-Identifier: GPL-3.0-or-later
"""Combining runs: a net that is open in the best state but complete in another run of the same board is taken over from that run.

Every closure run leaves many judged states of the same board (stage, fine and window directories). A different seed or remedy leaves a different few
connections open, so the nets that one state cannot close are often closed in another. `merge_closure` is a crossover of two states (the idea of the
crossover in genetic algorithms, applied to whole nets): the open net comes from the donor together with every net of the best state that its copper would
touch, and so on until nothing touches, so the result is made of two consistent halves and nothing has to be ripped up. closure.py `combine` ranks the merges
by the open connections they should have (the best state's outside the taken nets, the donor's inside), lets the router finish what is still open, and KiCad
decides."""
from __future__ import annotations

import json
import math
import pathlib
import re
import sys

sys.path.insert(0, str(pathlib.Path(__file__).parent))
import pcbtext

NUM = r"(-?[0-9.]+)"


def net_table(text: str) -> dict[int, str]:
    """id -> name of the board's nets (`(net 12 "GND")` records)."""
    return {int(m[1]): m[2] if m[2] is not None else m[3] for m in re.finditer(r'\(net (\d+) (?:"([^"]*)"|([^\s")]+))\)', text)}


def copper_geom(text: str) -> list[dict]:
    """Every segment and via with what a conflict test needs: kind, span, net id, points, width (segment) or size (via), layer (segment)."""
    out = []
    for kind, a, b, net, pts in pcbtext.copper(text):
        s = text[a:b]
        if kind == "segment":
            w = re.search(rf"\(width {NUM}\)", s)
            ly = re.search(r'\(layer "([^"]*)"\)', s)
            out.append({"kind": kind, "a": a, "b": b, "net": net, "pts": pts, "w": float(w[1]) if w else 0.0, "layer": ly[1] if ly else ""})
        else:
            sz = re.search(rf"\(size {NUM}\)", s)
            out.append({"kind": kind, "a": a, "b": b, "net": net, "pts": pts, "w": float(sz[1]) if sz else 0.0, "layer": ""})
    return out


def _cross(ax, ay, bx, by, cx, cy):
    return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)


def _pt_seg(px, py, ax, ay, bx, by) -> float:
    dx, dy = bx - ax, by - ay
    l2 = dx * dx + dy * dy
    t = 0.0 if l2 == 0 else max(0.0, min(1.0, ((px - ax) * dx + (py - ay) * dy) / l2))
    return math.hypot(px - (ax + t * dx), py - (ay + t * dy))


def seg_seg(a, b, c, d) -> float:
    """Distance between segments ab and cd (0 when they cross)."""
    d1, d2 = _cross(*c, *d, *a), _cross(*c, *d, *b)
    d3, d4 = _cross(*a, *b, *c), _cross(*a, *b, *d)
    if ((d1 > 0) != (d2 > 0)) and ((d3 > 0) != (d4 > 0)) and d1 != 0 and d2 != 0 and d3 != 0 and d4 != 0:
        return 0.0
    return min(_pt_seg(*a, *c, *d), _pt_seg(*b, *c, *d), _pt_seg(*c, *a, *b), _pt_seg(*d, *a, *b))


def conflicts(new: dict, old: dict, margin: float) -> bool:
    """Would the copper item `new` come closer than `margin` to `old` (edge to edge)? A via is on every copper layer; two segments only conflict on one layer."""
    nv, ov = new["kind"] == "via", old["kind"] == "via"
    if not nv and not ov and new["layer"] != old["layer"]:
        return False
    need = new["w"] / 2 + old["w"] / 2 + margin
    if nv and ov:
        return math.hypot(new["pts"][0][0] - old["pts"][0][0], new["pts"][0][1] - old["pts"][0][1]) < need
    if nv or ov:
        v, sg = (new, old) if nv else (old, new)
        return _pt_seg(*v["pts"][0], *sg["pts"][0], *sg["pts"][1]) < need
    return seg_seg(*new["pts"], *old["pts"]) < need


def pieces(items: list[dict]) -> dict[int, set[int]]:
    """Connected pieces of the same net (copper sharing an end point to 1 um): item index -> the set of item indices of its piece."""
    key = lambda p: (round(p[0], 3), round(p[1], 3))
    by_pt: dict[tuple, list[int]] = {}
    for k, it in enumerate(items):
        for p in it["pts"]:
            by_pt.setdefault((it["net"], *key(p)), []).append(k)
    comp: dict[int, set[int]] = {}
    for k0 in range(len(items)):
        if k0 in comp:
            continue
        cur, todo = set(), [k0]
        while todo:
            k = todo.pop()
            if k in cur:
                continue
            cur.add(k)
            for p in items[k]["pts"]:
                todo += [j for j in by_pt.get((items[k]["net"], *key(p)), []) if j not in cur]
        for k in cur:
            comp[k] = cur
    return comp


def drc_open_counts(drc_json: pathlib.Path) -> dict[str, int] | None:
    """Open connections per net name in a KiCad DRC report (an unconnected item counts for the first net it names); None when the file is unreadable."""
    try:
        d = json.loads(drc_json.read_text())
    except (OSError, ValueError):
        return None
    out: dict[str, int] = {}
    for u in d.get("unconnected_items", []):
        for i in u["items"]:
            m = re.search(r"\[([^\]]*)\]", i["description"])
            if m:
                out[m.group(1)] = out.get(m.group(1), 0) + 1
                break
    return out


def drc_open_nets(drc_json: pathlib.Path) -> tuple[int, set[str]] | None:
    """(open connections, names of the nets that have one) in a KiCad DRC report; None when the file is unreadable."""
    try:
        d = json.loads(drc_json.read_text())
    except (OSError, ValueError):
        return None
    out = set()
    for u in d.get("unconnected_items", []):
        for i in u["items"]:
            m = re.search(r"\[([^\]]*)\]", i["description"])
            if m:
                out.add(m.group(1))
                break
    return len(d.get("unconnected_items", [])), out


def donors(board_dir: pathlib.Path, skip: pathlib.Path | None, moves: dict, limit: int) -> list[tuple[int, pathlib.Path, dict]]:
    """Judged states (routed.kicad_pcb with routed.drc.json) of the board's closure runs that have the same parts moved, fewest open first: (open, file, open count per net)."""
    found = []
    per_run = max(8, limit // 12)  # every run keeps its best states only: the runs of one family all share the same stuck nets, a diverse pool needs the others too
    for rdir in sorted(board_dir.glob("closure-*")):
        mine = []
        cj = rdir / "closure.json"
        if not cj.is_file():
            continue
        try:
            if json.loads(cj.read_text()).get("moves", {}) != moves:
                continue
        except (OSError, ValueError):
            continue
        for p in list(rdir.glob("*/routed.drc.json")) + list(rdir.glob("routed.drc.json")):
            f = p.parent / "routed.kicad_pcb"
            if not f.is_file() or (skip is not None and f.resolve() == skip.resolve()):
                continue
            r = drc_open_counts(p)
            if r is not None:
                mine.append((sum(r.values()), f, r))
        found += sorted(mine, key=lambda t: (t[0], str(t[1])))[:per_run]
    found.sort(key=lambda t: (t[0], str(t[1])))
    return found[:limit]


def nets_near(text: str, points: list[tuple[float, float]], radius: float) -> set[str]:
    """Names of the nets that have copper (a segment or a via, its centre line) within `radius` mm of any of `points`: the seeds of a region crossover."""
    nt = net_table(text)
    out: set[str] = set()
    for it in copper_geom(text):
        nm = nt.get(it["net"], "")
        if not nm or nm in out:
            continue
        for (px, py) in points:
            d = math.hypot(px - it["pts"][0][0], py - it["pts"][0][1]) if it["kind"] == "via" else _pt_seg(px, py, *it["pts"][0], *it["pts"][1])
            if d <= radius + it["w"] / 2:
                out.add(nm)
                break
    return out


def merge_closure(base: str, donor: str, seeds, max_nets: int = 60, margin: float = 0.12):
    """The crossover of two judged states: the nets in `seeds` come from `donor` together with every net of `base` their copper would touch (and so on until
    nothing touches), so the result is made of two consistent halves and nothing has to be ripped and routed again. Returns (text, nets taken) or None when
    a seed has no copper in the donor or the set grows past `max_nets`."""
    bt, dt = net_table(base), net_table(donor)
    b_id = {n: i for i, n in bt.items()}
    d_id = {n: i for i, n in dt.items()}
    bi, di = copper_geom(base), copper_geom(donor)
    d_by = {}
    for it in di:
        d_by.setdefault(dt.get(it["net"], ""), []).append(it)
    cell = 2.0
    grid: dict[tuple, list[int]] = {}
    for k, it in enumerate(bi):
        xs = [p[0] for p in it["pts"]]
        ys = [p[1] for p in it["pts"]]
        r = it["w"] / 2 + margin
        for gx in range(int((min(xs) - r) // cell), int((max(xs) + r) // cell) + 1):
            for gy in range(int((min(ys) - r) // cell), int((max(ys) + r) // cell) + 1):
                grid.setdefault((gx, gy), []).append(k)
    take = set(seeds)
    if any(n not in d_by or n not in b_id for n in take):
        return None
    todo = list(take)
    while todo:
        n = todo.pop()
        for new in d_by.get(n, []):
            xs = [p[0] for p in new["pts"]]
            ys = [p[1] for p in new["pts"]]
            r = new["w"] / 2 + margin
            near = set()
            for gx in range(int((min(xs) - r) // cell), int((max(xs) + r) // cell) + 1):
                for gy in range(int((min(ys) - r) // cell), int((max(ys) + r) // cell) + 1):
                    near.update(grid.get((gx, gy), ()))
            for k in near:
                old = bi[k]
                nm = bt.get(old["net"], "")
                if nm in take or old["net"] == 0:
                    continue
                if conflicts(new, old, margin):
                    if nm not in d_by:
                        return None  # the donor has no copper of that net: it would be left open
                    take.add(nm)
                    todo.append(nm)
                    if len(take) > max_nets:
                        return None
    drop = [it for it in bi if bt.get(it["net"], "") in take]
    text = base
    for it in sorted(drop, key=lambda i: -i["a"]):
        a, b = it["a"], it["b"]
        j = a
        while j > 0 and text[j - 1] in " \t":
            j -= 1
        if j > 0 and text[j - 1] == "\n":
            j -= 1
        text = text[:j] + text[b:]
    forms = [re.sub(r"\(net \d+\)", f"(net {b_id[n]})", donor[it["a"]:it["b"]], count=1) for n in sorted(take) for it in d_by[n]]
    end = text.rstrip().rfind(")")
    text = text[:end].rstrip("\n") + "\n" + "\n".join("  " + f for f in forms) + "\n)\n"
    return text, sorted(take)
