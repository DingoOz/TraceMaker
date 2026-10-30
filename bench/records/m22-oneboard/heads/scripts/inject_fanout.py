# inject_fanout.py ROUTED UNROUTED OUT X0 Y0 X1 Y1 [clip]: copy the routed board's segments and vias inside the box into the unrouted board.
# A segment with one end inside is cut at the box edge (clip) or kept whole (no clip); segments wholly outside are left out.
import re, sys
src, dst, out = sys.argv[1:4]; x0, y0, x1, y1 = map(float, sys.argv[4:8]); clip = len(sys.argv) > 8 and sys.argv[8] == 'clip'
txt = re.sub(r'\s+', ' ', open(src).read())
inside = lambda x, y: x0 <= x <= x1 and y0 <= y <= y1
seg = re.compile(r'\(segment \(start ([-\d.]+) ([-\d.]+)\) \(end ([-\d.]+) ([-\d.]+)\) \(width ([\d.]+)\) \(layer "?([\w.]+)"?\) \(net (\d+)\)')
via = re.compile(r'\(via \(at ([-\d.]+) ([-\d.]+)\) \(size ([\d.]+)\)( \(drill [\d.]+\))? \(layers "?([\w.]+)"? "?([\w.]+)"?\) \(net (\d+)\)')
add = []; ns = nv = 0
def cut(ax, ay, bx, by):  # point where a (inside) -> b (outside) leaves the box
    t = 1.0
    for (p, q, lo, hi) in ((ax, bx, x0, x1), (ay, by, y0, y1)):
        if q < lo: t = min(t, (lo - p) / (q - p))
        if q > hi: t = min(t, (hi - p) / (q - p))
    return ax + (bx - ax) * t, ay + (by - ay) * t
for m in seg.finditer(txt):
    ax, ay, bx, by, w = map(float, m.groups()[:5]); layer, net = m.group(6), m.group(7)
    ia, ib = inside(ax, ay), inside(bx, by)
    if not (ia or ib): continue
    if clip and not (ia and ib):
        if ib: ax, ay, bx, by = bx, by, ax, ay
        bx, by = cut(ax, ay, bx, by)
    add.append(f'  (segment (start {ax:.4f} {ay:.4f}) (end {bx:.4f} {by:.4f}) (width {w}) (layer {layer}) (net {net}))'); ns += 1
for m in via.finditer(txt):
    x, y, s = map(float, m.groups()[:3])
    if inside(x, y):
        add.append(f'  (via (at {x} {y}) (size {s}){m.group(4) or ""} (layers {m.group(5)} {m.group(6)}) (net {m.group(7)}))'); nv += 1
d = open(dst).read().rstrip(); assert d.endswith(')')
open(out, 'w').write(d[:-1] + '\n'.join(add) + '\n)\n')
print(f'{ns} segments, {nv} vias')
