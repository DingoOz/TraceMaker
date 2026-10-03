#!/usr/bin/env python3
"""Write a Specctra .dsn for a re-placed board by moving the `(place ...)` records of the original board's .dsn
(kicad-cli cannot export Specctra). The coordinate map (scale, y flip, offset, rotation) is checked against the
original board first: every component of the original .kicad_pcb must map onto its .dsn record exactly.

  bench/dsn_place.py original.kicad_pcb original.dsn placed.kicad_pcb -o placed.dsn
"""
import argparse
import re


def footprints(path: str) -> dict:
    """reference -> (x mm, y mm, angle deg, side) from (footprint|module) blocks (balanced-paren scan)."""
    t = open(path, errors="replace").read()
    out = {}
    for m in re.finditer(r"\((footprint|module)\s", t):
        depth, i = 0, m.start()
        while True:
            depth += (t[i] == "(") - (t[i] == ")")
            i += 1
            if depth == 0:
                break
        blk = t[m.start():i]
        ref = re.search(r'\(fp_text reference "?([^"\s)]+)|\(property "Reference" "([^"]+)"', blk)
        at = re.search(r"\(at\s+(-?[\d.]+)\s+(-?[\d.]+)(?:\s+(-?[\d.]+))?\)", blk)
        side = "back" if re.search(r'\(layer "?B\.Cu"?\)|\(layer Back\)', blk[:300]) else "front"
        if ref and at:
            out[ref.group(1) or ref.group(2)] = (float(at.group(1)), float(at.group(2)), float(at.group(3) or 0), side)
    return out


def norm(a: float) -> float:
    a = a % 360
    return a - 360 if a > 180 else a


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("original")
    ap.add_argument("dsn")
    ap.add_argument("placed")
    ap.add_argument("-o", "--out", required=True)
    a = ap.parse_args()
    dsn = open(a.dsn).read()
    unit = re.search(r"\(resolution\s+(\w+)\s+(\d+)\)", dsn)
    per_mm = {"um": 1000.0, "mm": 1.0, "mil": 1 / 0.0254, "inch": 1 / 25.4}[unit.group(1)]
    orig, new = footprints(a.original), footprints(a.placed)
    place = re.compile(r"\(place\s+(\"[^\"]+\"|\S+)\s+(-?[\d.]+)\s+(-?[\d.]+)\s+(front|back)\s+(-?[\d.]+)")
    for m in place.finditer(dsn):
        ref = m.group(1).strip('"')
        x, y, ang, side = orig[ref]
        want = (x * per_mm, -y * per_mm)
        got = (float(m.group(2)), float(m.group(3)))
        if abs(want[0] - got[0]) > 1 or abs(want[1] - got[1]) > 1 or side != m.group(4):
            raise SystemExit(f"{ref}: original board does not map onto the .dsn ({want} vs {got}, {side} vs {m.group(4)})")
        if side == "front" and abs(norm(ang - float(m.group(5)))) > 0.01:
            raise SystemExit(f"{ref}: rotation map differs ({ang} vs {m.group(5)})")
    moved = 0

    def sub(m):
        nonlocal moved
        ref = m.group(1).strip('"')
        x, y, ang, side = new[ref]
        ox, oy, oang, _ = orig[ref]
        if (x, y, ang) != (ox, oy, oang):
            moved += 1
        rot = norm(ang) if side == "front" else norm(float(m.group(5)) + (ang - oang))
        return f"(place {m.group(1)} {x * per_mm:.0f} {-y * per_mm:.0f} {side} {rot:g}"
    open(a.out, "w").write(place.sub(sub, dsn))
    print(f"{a.out}: {moved} component(s) moved")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
