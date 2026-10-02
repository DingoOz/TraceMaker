#!/usr/bin/env python3
"""Import a Specctra session (.ses, e.g. from Freerouting) into a copy of a .kicad_pcb.

Appends the session's wires as `segment` items and its vias as `via` items to the board text, so KiCad's DRC
and TraceMaker's quality metrics can judge Freerouting's routing exactly like TraceMaker's.

  bench/ses_import.py unrouted.kicad_pcb routed.ses -o routed.kicad_pcb

Units: the session's `resolution` (e.g. `um 10` = 0.1 um per unit); y is negated (Specctra y points up).
Via size and drill come from the padstack's circle and the KiCad-generated name `Via[0-1]_800:500_um`.
"""
import argparse
import pathlib
import re
import sys


def tokenize(s):
    i, n = 0, len(s)
    while i < n:
        c = s[i]
        if c in " \t\r\n":
            i += 1
        elif c in "()":
            yield c
            i += 1
        elif c == '"':
            j = i + 1
            while j < n and s[j] != '"':
                j += 1
            yield s[i + 1:j]
            i = j + 1
        else:
            j = i
            while j < n and s[j] not in ' \t\r\n()"':
                j += 1
            yield s[i:j]
            i = j


def parse(s):
    stack, cur = [], []
    for t in tokenize(s):
        if t == "(":
            stack.append(cur)
            cur = []
        elif t == ")":
            done = cur
            cur = stack.pop()
            cur.append(done)
        else:
            cur.append(t)
    return cur[0] if cur else []


def find(node, key):
    return [c for c in node if isinstance(c, list) and c and c[0] == key]


def walk(node, key):
    if isinstance(node, list):
        if node and node[0] == key:
            yield node
        for c in node:
            yield from walk(c, key)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("board")
    ap.add_argument("ses")
    ap.add_argument("-o", "--output", required=True)
    a = ap.parse_args()

    board = pathlib.Path(a.board).read_text()
    ses = parse(pathlib.Path(a.ses).read_text())
    routes = next(walk(ses, "routes"), None)
    if routes is None:
        sys.exit("no routes in session")
    res = next(walk(routes, "resolution"), ["resolution", "um", "10"])
    unit_mm = {"um": 1e-3, "mm": 1.0, "mil": 0.0254, "inch": 25.4}[res[1].lower()] / float(res[2])
    mm = lambda v: float(v) * unit_mm  # noqa: E731

    # Padstacks: name -> (diameter mm, drill mm, layers)
    pads = {}
    for ps in walk(routes, "padstack"):
        name = ps[1]
        dia = None
        layers = []
        for sh in find(ps, "shape"):
            for circ in find(sh, "circle"):
                layers.append(circ[1])
                dia = mm(circ[2])
        drill = None
        m = re.search(r"_(\d+(?:\.\d+)?):(\d+(?:\.\d+)?)_(um|mil)", name)
        if m:
            f = 1e-3 if m.group(3) == "um" else 0.0254
            dia = dia or float(m.group(1)) * f
            drill = float(m.group(2)) * f
        pads[name] = (dia or 0.8, drill or 0.4, layers)

    # Net syntax of the board: numbered table (KiCad <= 9) or names only (KiCad 10).
    table = {name: num for num, name in re.findall(r'^\s*\(net (\d+) "?([^"\n)]*)"?\)\s*$', board, re.M)}
    numbered = bool(table)
    copper = re.findall(r'^\s*\(\d+ "?([^"\s)]+)"? (?:signal|power|mixed|jumper)', board, re.M)
    first, last = (copper[0], copper[-1]) if copper else ("F.Cu", "B.Cu")

    def net_ref(name):
        if numbered:
            return f"(net {table.get(name, 0)})"
        return f'(net "{name}")'

    items = []
    nseg = nvia = 0
    for net in walk(routes, "net"):
        name = net[1]
        for wire in find(net, "wire"):
            for path in find(wire, "path"):
                layer, width = path[1], mm(path[2])
                xy = [mm(v) for v in path[3:] if not isinstance(v, list)]
                pts = [(xy[i], -xy[i + 1]) for i in range(0, len(xy) - 1, 2)]
                for (x1, y1), (x2, y2) in zip(pts, pts[1:]):
                    if (x1, y1) == (x2, y2):
                        continue
                    items.append(f'  (segment (start {x1:.6f} {y1:.6f}) (end {x2:.6f} {y2:.6f}) (width {width:.6f}) '
                                 f'(layer "{layer}") {net_ref(name)})')
                    nseg += 1
        for via in find(net, "via"):
            dia, drill, layers = pads.get(via[1], (0.8, 0.4, []))
            x, y = mm(via[2]), -mm(via[3])
            l0, l1 = (layers[0], layers[-1]) if len(layers) >= 2 else (first, last)
            items.append(f'  (via (at {x:.6f} {y:.6f}) (size {dia:.6f}) (drill {drill:.6f}) (layers "{l0}" "{l1}") {net_ref(name)})')
            nvia += 1
    end = board.rstrip().rfind(")")
    pathlib.Path(a.output).write_text(board[:end] + "\n".join(items) + "\n" + board[end:])
    print(f"imported {nseg} segments, {nvia} vias -> {a.output}")


if __name__ == "__main__":
    main()
