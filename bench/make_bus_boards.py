#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Synthetic boards with buses (doc 05 §38): there is no public bus benchmark, so these small boards are generated.

  bench/make_bus_boards.py OUT_DIR [NAME ...]

Writes OUT_DIR/<name>/<name>.kicad_pcb with its .kicad_pro (and .kicad_dru where the case has custom rules) and
OUT_DIR/manifest.json, which gives every board its bus options and what the case is. The files are written directly
as KiCad 10 s-expressions, with identifiers derived from a counter: the same call gives the same bytes on any machine.

Cases:
  p2p8      8-bit bus between two facing ICs (mirror-image pin columns), two layers; a mounting hole and two
            capacitors stand in the channel and the ICs are offset, so the lanes come out with different lengths.
  p2p16     16-bit bus from an IC on the front to one mounted on the back (its footprint mirrored), two layers:
            every lane needs a via.
  lcd       12-bit display bus from a QFP to a through-hole connector on four layers; a rule area across the board
            takes no tracks on the front layer, which forces a layer change.
  spi       SPI fan-out: three shared nets from a controller to three devices (four pads per net), two layers.
  flyby     8 address lines visiting three memories in sequence from a controller, four layers.
  skewrule  as p2p8, with a net class BUS and a .kicad_dru skew rule on it; meant for detection (--buses).
  twist     as p2p8 with the pin order reversed at one end, so every lane must cross every other (information:
            what the grouping does when a bus cannot stay planar; not part of the gate).
"""
import json
import pathlib
import sys


class Board:
    def __init__(self, name, width, height, layers=2):
        self.name, self.w, self.h, self.layers = name, width, height, layers
        self.items = []
        self.n = 0
        self.x0, self.y0 = 100.0, 100.0  # board origin on the sheet

    def uuid(self):
        self.n += 1
        return f"00000000-0000-4000-8000-{self.n:012x}"

    @staticmethod
    def num(v):
        s = f"{v:.4f}".rstrip("0").rstrip(".")
        return "0" if s in ("-0", "") else s

    def footprint(self, ref, x, y, pads, back=False, lib="bus"):
        """pads: (number, dx, dy, w, h, net, kind) with kind 'smd' or 'th:<drill>'."""
        side = "B" if back else "F"
        n = self.num
        out = [f'\t(footprint "{lib}:{ref}"', f'\t\t(layer "{side}.Cu")', f'\t\t(uuid "{self.uuid()}")',
               f"\t\t(at {n(self.x0 + x)} {n(self.y0 + y)})",
               f'\t\t(property "Reference" "{ref}"\n\t\t\t(at 0 0 0)\n\t\t\t(layer "{side}.Fab")\n\t\t\t(hide yes)\n\t\t\t(uuid "{self.uuid()}")\n'
               f"\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1 1)\n\t\t\t\t\t(thickness 0.15)\n\t\t\t\t)\n\t\t\t)\n\t\t)",
               f'\t\t(property "Value" "{lib}"\n\t\t\t(at 0 0 0)\n\t\t\t(layer "{side}.Fab")\n\t\t\t(hide yes)\n\t\t\t(uuid "{self.uuid()}")\n'
               f"\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1 1)\n\t\t\t\t\t(thickness 0.15)\n\t\t\t\t)\n\t\t\t)\n\t\t)",
               "\t\t(attr smd)" if all(p[6] == "smd" for p in pads) else "\t\t(attr through_hole)"]
        for num, dx, dy, w, h, net, kind in pads:
            if kind == "smd":
                head = f'\t\t(pad "{num}" smd roundrect\n\t\t\t(at {n(dx)} {n(dy)})\n\t\t\t(size {n(w)} {n(h)})\n' \
                       f'\t\t\t(layers "{side}.Cu" "{side}.Mask" "{side}.Paste")\n\t\t\t(roundrect_rratio 0.25)'
            else:
                head = f'\t\t(pad "{num}" thru_hole circle\n\t\t\t(at {n(dx)} {n(dy)})\n\t\t\t(size {n(w)} {n(h)})\n' \
                       f'\t\t\t(drill {kind[3:]})\n\t\t\t(layers "*.Cu" "*.Mask")\n\t\t\t(remove_unused_layers no)'
            if net:
                head += f'\n\t\t\t(net "{net}")'
            out.append(head + f'\n\t\t\t(uuid "{self.uuid()}")\n\t\t)')
        out.append("\t\t(embedded_fonts no)\n\t)")
        self.items.append("\n".join(out))

    def hole(self, ref, x, y, d=3.2):
        n = self.num
        self.items.append(
            f'\t(footprint "bus:{ref}"\n\t\t(layer "F.Cu")\n\t\t(uuid "{self.uuid()}")\n\t\t(at {n(self.x0 + x)} {n(self.y0 + y)})\n'
            f'\t\t(property "Reference" "{ref}"\n\t\t\t(at 0 0 0)\n\t\t\t(layer "F.Fab")\n\t\t\t(hide yes)\n\t\t\t(uuid "{self.uuid()}")\n'
            f"\t\t\t(effects\n\t\t\t\t(font\n\t\t\t\t\t(size 1 1)\n\t\t\t\t\t(thickness 0.15)\n\t\t\t\t)\n\t\t\t)\n\t\t)\n"
            f"\t\t(attr exclude_from_pos_files exclude_from_bom)\n"
            f'\t\t(pad "" np_thru_hole circle\n\t\t\t(at 0 0)\n\t\t\t(size {n(d)} {n(d)})\n\t\t\t(drill {n(d)})\n\t\t\t(layers "*.Cu" "*.Mask")\n'
            f'\t\t\t(uuid "{self.uuid()}")\n\t\t)\n\t\t(embedded_fonts no)\n\t)')

    def keepout(self, name, layer, x0, y0, x1, y1):
        n = self.num
        X0, Y0, X1, Y1 = self.x0 + x0, self.y0 + y0, self.x0 + x1, self.y0 + y1
        self.items.append(
            f'\t(zone\n\t\t(layer "{layer}")\n\t\t(uuid "{self.uuid()}")\n\t\t(name "{name}")\n\t\t(hatch edge 0.5)\n'
            f"\t\t(connect_pads\n\t\t\t(clearance 0)\n\t\t)\n\t\t(min_thickness 0.25)\n"
            f"\t\t(keepout\n\t\t\t(tracks not_allowed)\n\t\t\t(vias not_allowed)\n\t\t\t(pads allowed)\n\t\t\t(copperpour allowed)\n\t\t\t(footprints allowed)\n\t\t)\n"
            f'\t\t(placement\n\t\t\t(enabled no)\n\t\t\t(sheetname "")\n\t\t)\n'
            f"\t\t(fill\n\t\t\t(thermal_gap 0.5)\n\t\t\t(thermal_bridge_width 0.5)\n\t\t\t(island_removal_mode 0)\n\t\t)\n"
            f"\t\t(polygon\n\t\t\t(pts\n\t\t\t\t(xy {n(X0)} {n(Y0)}) (xy {n(X1)} {n(Y0)}) (xy {n(X1)} {n(Y1)}) (xy {n(X0)} {n(Y1)})\n\t\t\t)\n\t\t)\n\t)")

    # ---- parts ----
    def dual(self, ref, x, y, left, right, pitch=1.27, span=5.4, pad=(1.5, 0.6), back=False):
        """Two pad columns; `left` and `right` are net names top to bottom ('' = unconnected)."""
        pads = []
        rows = max(len(left), len(right))
        top = -(rows - 1) * pitch / 2
        for i, net in enumerate(left):
            pads.append((str(i + 1), -span / 2, top + i * pitch, pad[0], pad[1], net, "smd"))
        for i, net in enumerate(right):
            pads.append((str(len(left) + i + 1), span / 2, top + i * pitch, pad[0], pad[1], net, "smd"))
        self.footprint(ref, x, y, pads, back=back)

    def quad(self, ref, x, y, sides, pitch=0.65, body=10.0, pad=(1.4, 0.38)):
        """Four pad rows: sides = (left, bottom, right, top) net lists, each top to bottom or left to right."""
        pads, k = [], 1
        left, bottom, right, top = sides
        for names, vertical, sign in ((left, True, -1), (bottom, False, 1), (right, True, 1), (top, False, -1)):
            first = -(len(names) - 1) * pitch / 2
            for i, net in enumerate(names):
                if vertical:
                    pads.append((str(k), sign * body / 2, first + i * pitch, pad[0], pad[1], net, "smd"))
                else:
                    pads.append((str(k), first + i * pitch, sign * body / 2, pad[1], pad[0], net, "smd"))
                k += 1
        self.footprint(ref, x, y, pads)

    def header(self, ref, x, y, nets, pitch=2.54, vertical=True, rows=1):
        pads = []
        per = (len(nets) + rows - 1) // rows
        first = -(per - 1) * pitch / 2
        for i, net in enumerate(nets):
            r, k = i % rows, i // rows
            a, c = first + k * pitch, (r - (rows - 1) / 2) * pitch
            pads.append((str(i + 1), c if vertical else a, a if vertical else c, 1.7, 1.7, net, "th:1"))
        self.footprint(ref, x, y, pads)

    def cap(self, ref, x, y, net_a, net_b, vertical=False):
        d = 0.95
        pads = [("1", 0 if vertical else -d, -d if vertical else 0, 0.9, 0.95, net_a, "smd"),
                ("2", 0 if vertical else d, d if vertical else 0, 0.9, 0.95, net_b, "smd")]
        self.footprint(ref, x, y, pads)

    # ---- output ----
    def text(self):
        n = self.num
        cu = ['(0 "F.Cu" signal)'] + ['(%d "In%d.Cu" signal)' % (2 + 2 * i, i) for i in range(1, self.layers - 1)] + ['(2 "B.Cu" signal)']
        others = ['(13 "F.Paste" user)', '(15 "B.Paste" user)', '(5 "F.SilkS" user "F.Silkscreen")', '(7 "B.SilkS" user "B.Silkscreen")',
                  '(1 "F.Mask" user)', '(3 "B.Mask" user)', '(25 "Edge.Cuts" user)', '(27 "Margin" user)', '(31 "F.CrtYd" user "F.Courtyard")',
                  '(29 "B.CrtYd" user "B.Courtyard")', '(35 "F.Fab" user)', '(33 "B.Fab" user)']
        head = ['(kicad_pcb', '\t(version 20260206)', '\t(generator "pcbnew")', '\t(generator_version "10.0")',
                '\t(general\n\t\t(thickness 1.6)\n\t\t(legacy_teardrops no)\n\t)', '\t(paper "A4")',
                "\t(layers\n" + "\n".join("\t\t" + l for l in cu + others) + "\n\t)",
                "\t(setup\n\t\t(pad_to_mask_clearance 0)\n\t\t(allow_soldermask_bridges_in_footprints no)\n"
                "\t\t(tenting\n\t\t\t(front yes)\n\t\t\t(back yes)\n\t\t)\n\t)"]
        edge = (f'\t(gr_rect\n\t\t(start {n(self.x0)} {n(self.y0)})\n\t\t(end {n(self.x0 + self.w)} {n(self.y0 + self.h)})\n'
                f'\t\t(stroke\n\t\t\t(width 0.05)\n\t\t\t(type default)\n\t\t)\n\t\t(fill no)\n\t\t(layer "Edge.Cuts")\n\t\t(uuid "{self.uuid()}")\n\t)')
        return "\n".join(head + self.items + [edge, "\t(embedded_fonts no)", ")"]) + "\n"


def project(name, classes=(), patterns=()):
    default = {"name": "Default", "clearance": 0.15, "track_width": 0.2, "via_diameter": 0.6, "via_drill": 0.3, "priority": 2147483647}
    return {"meta": {"filename": f"{name}.kicad_pro", "version": 3},
            "board": {"design_settings": {"rules": {
                "min_clearance": 0.15, "min_track_width": 0.15, "min_via_diameter": 0.5, "min_through_hole_diameter": 0.3,
                "min_via_annular_width": 0.1, "min_hole_clearance": 0.25, "min_hole_to_hole": 0.25, "min_copper_edge_clearance": 0.3,
                "allow_blind_buried_vias": False, "allow_microvias": False}}},
            "net_settings": {"meta": {"version": 4}, "classes": [default, *classes],
                             "netclass_patterns": [{"netclass": c, "pattern": p} for p, c in patterns]}}


def bits(stem, n, first=0):
    return [f"{stem}{i}" for i in range(first, first + n)]


def p2p(name, n, reverse=False, back=False, classes=(), patterns=(), dru=None, stem="/D"):
    """Two ICs facing each other; the bus leaves U1's right column and enters U2's left column."""
    rows = n + 2
    h = max(26.0, rows * 1.27 + 12)
    b = Board(name, 44.0, h)
    d = bits(stem, n)
    other = ["/CTL_A", "/CTL_B"]
    b.dual("U1", 8.0, h / 2 - 3.0, ["/VCC"] + [""] * (rows - 2) + ["/GND"], other[:1] + d + other[1:])
    far = list(reversed(d)) if reverse else d
    b.dual("U2", 36.0, h / 2 + 3.0, other[:1] + far + other[1:], ["/VCC"] + [""] * (rows - 2) + ["/GND"], back=back)
    # In the channel: a mounting hole on the straight line of the middle lanes, a capacitor above and one below.
    b.hole("H1", 22.0, h / 2, 3.2)
    b.cap("C1", 22.0, h / 2 - 6.0, "/VCC", "/GND")
    b.cap("C2", 22.0, h / 2 + 6.5, "/VCC", "/GND")
    return b, project(name, classes, patterns), dru


def lcd(name):
    """A QFP's right side drives a 12-pin through-hole header; a rule area on the front layer lies across the way."""
    b = Board(name, 52.0, 40.0, layers=4)
    d = bits("/LCD_D", 12)
    quiet = [""] * 12
    b.quad("U1", 13.0, 20.0, (["/VCC"] + quiet[1:11] + ["/GND"], quiet, d, quiet))
    b.header("J1", 45.0, 20.0, d, pitch=2.54)
    b.keepout("NO_FRONT", "F.Cu", 26.0, 0.5, 31.0, 39.5)
    b.cap("C1", 13.0, 7.0, "/VCC", "/GND")
    b.cap("C2", 13.0, 33.0, "/VCC", "/GND")
    return b, project(name), None


def spi(name):
    """Controller on the left, three devices in a column on the right; SCK, MOSI and MISO go to all of them."""
    b = Board(name, 46.0, 44.0)
    shared = ["/SPI_SCK", "/SPI_MOSI", "/SPI_MISO"]
    b.dual("U1", 9.0, 22.0, ["/VCC", "", "", "", "", "/GND"], shared + ["/CS1", "/CS2", "/CS3"])
    for i in range(3):
        y = 8.0 + 14.0 * i
        b.dual(f"U{i + 2}", 35.0, y, [f"/CS{i + 1}"] + shared, ["/VCC", "", "", "/GND"])
    b.hole("H1", 22.0, 15.0, 3.2)
    b.cap("C1", 22.0, 30.0, "/VCC", "/GND", vertical=True)
    return b, project(name), None


def flyby(name):
    """A controller at the left end and three memories in a row; eight address lines visit each memory in turn."""
    b = Board(name, 70.0, 34.0, layers=4)
    a = bits("/A", 8)
    b.dual("U1", 8.0, 17.0, ["/VCC"] + [""] * 6 + ["/GND"], a)
    for i in range(3):
        b.dual(f"U{i + 2}", 26.0 + 16.0 * i, 17.0, a, ["/VCC"] + [""] * 6 + ["/GND"])
    b.cap("C1", 17.0, 4.0, "/VCC", "/GND")
    b.cap("C2", 49.0, 30.0, "/VCC", "/GND")
    return b, project(name), None


CASES = {
    "p2p8": (lambda: p2p("p2p8", 8), ["--bus", "DATA=/D[0-7]", "--bus-skew-mm", "0.5"], True),
    "p2p16": (lambda: p2p("p2p16", 16, back=True), ["--bus", "DATA=/D[0-15]", "--bus-skew-mm", "1"], True),
    "lcd": (lambda: lcd("lcd"), ["--bus", "LCD=/LCD_D*", "--bus-skew-mm", "1"], True),
    "spi": (lambda: spi("spi"), ["--bus", "SPI=/SPI_*", "--bus-skew-mm", "2"], True),
    "flyby": (lambda: flyby("flyby"), ["--bus", "ADDR=/A[0-7]", "--bus-skew-mm", "0.25"], True),
    "skewrule": (lambda: p2p("skewrule", 8, stem="/DQ",
                             classes=[{"name": "BUS", "clearance": 0.15, "track_width": 0.2, "via_diameter": 0.6, "via_drill": 0.3, "priority": 0}],
                             patterns=[("/DQ*", "BUS")],
                             dru='(version 1)\n(rule "bus_skew"\n\t(constraint skew (max 0.8mm))\n\t(condition "A.NetClass == \'BUS\'"))\n'),
                 ["--buses"], True),
    "twist": (lambda: p2p("twist", 8, reverse=True), ["--bus", "DATA=/D[0-7]", "--bus-skew-mm", "1"], False),
}


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 2
    out = pathlib.Path(sys.argv[1])
    names = sys.argv[2:] or list(CASES)
    manifest = []
    for name in names:
        make, args, gated = CASES[name]
        board, pro, dru = make()
        d = out / name
        d.mkdir(parents=True, exist_ok=True)
        (d / f"{name}.kicad_pcb").write_text(board.text())
        (d / f"{name}.kicad_pro").write_text(json.dumps(pro, indent=2) + "\n")
        if dru:
            (d / f"{name}.kicad_dru").write_text(dru)
        manifest.append({"name": name, "board": f"{name}/{name}.kicad_pcb", "bus_args": args, "gate": gated, "dru": bool(dru)})
    (out / "manifest.json").write_text(json.dumps(manifest, indent=1) + "\n")
    print(f"{len(manifest)} boards written to {out}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
