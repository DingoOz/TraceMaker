#!/usr/bin/env python3
"""M26 fan-out first: writes a quadrant dog-bone fan-out of the srj20 BGA into a copy of the board as ordinary copper.

Each ball with a net in ring >= SKIP (ring 0 = outer) gets a stub on F.Cu from its centre to the diagonal site half a pitch out
toward its quadrant's corner (away from the package centre), and a through via there. Every ball of a quadrant points the same
way, so the vias form a regular lattice with one track channel between neighbours on B.Cu (0.8 mm pitch: 0.5 mm gap, a 0.12 mm
track needs 0.32 mm). usage: fanout.py in.kicad_pcb out_dir skip
"""
import pathlib, re, shutil, sys, uuid

src, out, skip = pathlib.Path(sys.argv[1]), pathlib.Path(sys.argv[2]), int(sys.argv[3])
text = src.read_text()
fp = re.search(r'\(footprint "SRJ:bga_component".*?\n\t\)\n', text, re.S).group(0)
at = re.search(r'\n\t\t\(at ([-\d.]+) ([-\d.]+)\)', fp)
ox, oy = float(at.group(1)), float(at.group(2))
pads = []
for m in re.finditer(r'\(pad "(\d+)" smd \w+\s*\(at ([-\d.]+) ([-\d.]+)\).*?(?=\(pad |\Z)', fp, re.S):
    net = re.search(r'\(net "([^"]+)"\)', m.group(0))
    pads.append((float(m.group(2)), float(m.group(3)), net.group(1) if net else None))
xs = sorted({p[0] for p in pads}); ys = sorted({p[1] for p in pads})
pitch = round(xs[1] - xs[0], 4)
cx, cy = (xs[0] + xs[-1]) / 2, (ys[0] + ys[-1]) / 2
nx, ny = len(xs), len(ys)
items = []
n = 0
for x, y, net in pads:
    if not net:
        continue
    i, j = xs.index(x), ys.index(y)
    ring = min(i, j, nx - 1 - i, ny - 1 - j)
    if ring < skip:
        continue
    sx = 1 if x >= cx else -1
    sy = 1 if y >= cy else -1
    bx, by = ox + x, oy + y
    vx, vy = bx + sx * pitch / 2, by + sy * pitch / 2
    items.append(f'\t(segment\n\t\t(start {bx:.4f} {by:.4f})\n\t\t(end {vx:.4f} {vy:.4f})\n\t\t(width 0.12)\n\t\t(layer "F.Cu")\n\t\t(net "{net}")\n\t\t(uuid "{uuid.uuid5(uuid.NAMESPACE_URL, f"s{net}{x}{y}")}")\n\t)\n')
    items.append(f'\t(via\n\t\t(at {vx:.4f} {vy:.4f})\n\t\t(size 0.3)\n\t\t(drill 0.15)\n\t\t(layers "F.Cu" "B.Cu")\n\t\t(net "{net}")\n\t\t(uuid "{uuid.uuid5(uuid.NAMESPACE_URL, f"v{net}{x}{y}")}")\n\t)\n')
    n += 1
end = text.rstrip().rfind(')')
out.mkdir(parents=True, exist_ok=True)
(out / "board.kicad_pcb").write_text(text[:end] + "".join(items) + text[end:])
for ext in (".kicad_pro", ".kicad_prl"):
    f = src.with_suffix(ext)
    if f.exists():
        shutil.copy(f, out / ("board" + ext))
print(f"{src.parent.name}: {n} dog-bones (pitch {pitch}, {nx}x{ny}, skip {skip})")
