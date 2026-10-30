# synth_stubs.py GEO.json UNROUTED OUT REF LEN_MM [skipnets]: straight pad-layer stubs outward from every signal pin of REF,
# LEN_MM past the pad's outer end, cut short of any other net's pad (clearance 0.19 + half width), appended as copper.
import json, re, sys
geo, dst, out, ref, L = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], float(sys.argv[5]) * 1e6
skip = set(sys.argv[6].split(',')) if len(sys.argv) > 6 else set()
d = json.load(open(geo)); txt = open(dst).read()
num = {m.group(2): m.group(1) for m in re.finditer(r'\(net (\d+) "((?:[^"\\]|\\.)*)"\)', txt)}
num.update({m.group(2): m.group(1) for m in re.finditer(r'\(net (\d+) ([^\s"()]+)\)', txt)})
pads = d['pads']; u = [p for p in pads if p['ref'] == ref]
cx = sum(p['x'] for p in u) / len(u); cy = sum(p['y'] for p in u) / len(u)
W, C = 200000, 190000
def box(p):
    w, h = (p['h'], p['w']) if round(abs(p['angle'])) % 180 == 90 else (p['w'], p['h'])
    return p['x'] - w / 2, p['y'] - h / 2, p['x'] + w / 2, p['y'] + h / 2
add = []
for p in u:
    if not p['net'] or p['net'] in skip: continue
    x0, y0, x1, y1 = box(p)
    if x1 - x0 > 2e6 and y1 - y0 > 2e6: continue  # exposed pad
    horiz = (x1 - x0) > (y1 - y0)
    if horiz: sx, dx, sy, dy = (x1, 1, p['y'], 0) if p['x'] > cx else (x0, -1, p['y'], 0)
    else: sx, dx, sy, dy = (p['x'], 0, y1, 1) if p['y'] > cy else (p['x'], 0, y0, -1)
    t = L
    for q in pads:
        if q is p or q['net'] == p['net']: continue
        a0, b0, a1, b1 = box(q); m = C + W / 2
        a0 -= m; b0 -= m; a1 += m; b1 += m
        if dx:  # y within band, x ahead
            if b0 < sy < b1:
                s = (a0 - sx) if dx > 0 else (sx - a1)
                if s >= 0: t = min(t, s)
        else:
            if a0 < sx < a1:
                s = (b0 - sy) if dy > 0 else (sy - b1)
                if s >= 0: t = min(t, s)
    if t < 0.3e6: continue
    ex, ey = sx + dx * t, sy + dy * t
    add.append(f'  (segment (start {p["x"]/1e6:.4f} {p["y"]/1e6:.4f}) (end {ex/1e6:.4f} {ey/1e6:.4f}) (width 0.2) (layer {p["layers"][0]}) (net {num[p["net"]]}))')
D = txt.rstrip(); assert D.endswith(')')
open(out, 'w').write(D[:-1] + '\n'.join(add) + '\n)\n'); print(len(add), 'stubs')
