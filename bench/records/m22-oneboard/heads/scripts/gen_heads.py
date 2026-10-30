# gen_heads.py GEO.json UNROUTED OUT REF R_MM [STEP_MM] [BEND0_MM]: designer-like fan-out heads for a perimeter package.
# Each pin leaves straight, then bends 45 deg toward its destination; per side the pins form [turn low][straight][turn high]
# blocks (planar), and the bends are staggered so parallel diagonals keep track + clearance apart. Clipped at foreign pads and
# earlier heads.
import json, math, re, sys
geo, dst, out, ref, R = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], float(sys.argv[5])
STEP = float(sys.argv[6]) if len(sys.argv) > 6 else 0.1
B0 = float(sys.argv[7]) if len(sys.argv) > 7 else 0.3
W, C = 0.2, 0.19
import os
TH = float(os.environ.get('TH', '30')); BACK = float(os.environ.get('BACK', '0'))
d = json.load(open(geo)); txt = open(dst).read()
num = {m.group(2): m.group(1) for m in re.finditer(r'\(net (\d+) "((?:[^"\\]|\\.)*)"\)', txt)}
num.update({m.group(2): m.group(1) for m in re.finditer(r'\(net (\d+) ([^\s"()]+)\)', txt)})
def box(p):
    w, h = (p['h'], p['w']) if round(abs(p['angle'])) % 180 == 90 else (p['w'], p['h'])
    return p['x'] / 1e6 - w / 2e6, p['y'] / 1e6 - h / 2e6, p['x'] / 1e6 + w / 2e6, p['y'] / 1e6 + h / 2e6
pads = d['pads']; u = [p for p in pads if p['ref'] == ref and p['net']]
sig = [p for p in u if not ((box(p)[2] - box(p)[0]) > 2 and (box(p)[3] - box(p)[1]) > 2)]
X0 = min(box(p)[0] for p in sig); X1 = max(box(p)[2] for p in sig); Y0 = min(box(p)[1] for p in sig); Y1 = max(box(p)[3] for p in sig)
cx, cy = (X0 + X1) / 2, (Y0 + Y1) / 2
def side(p):
    a, b, c_, e = box(p)
    if (c_ - a) > (e - b): return ('R', (1, 0), (0, 1), c_) if p['x'] / 1e6 > cx else ('L', (-1, 0), (0, 1), a)
    return ('B', (0, 1), (1, 0), e) if p['y'] / 1e6 > cy else ('T', (0, -1), (1, 0), b)
ring = (X0 - R, Y0 - R, X1 + R, Y1 + R)
inring = lambda x, y: ring[0] <= x <= ring[2] and ring[1] <= y <= ring[3]
heads = {}  # side -> list of (pin, t, turn, dest)
for p in sig:
    others = [q for q in pads if q['net'] == p['net'] and q['ref'] != ref]
    if not others: continue
    q = min(others, key=lambda q: (q['x'] - p['x']) ** 2 + (q['y'] - p['y']) ** 2)
    qx, qy = q['x'] / 1e6, q['y'] / 1e6
    if inring(qx, qy): continue  # local connection: the router's
    s, n, t, edge = side(p); px, py = p['x'] / 1e6, p['y'] / 1e6
    dn = (qx - px) * n[0] + (qy - py) * n[1]; dt = (qx - px) * t[0] + (qy - py) * t[1]
    turn = 0 if abs(math.atan2(dt, max(dn, 1e-9))) < math.radians(TH) and dn > 0 else (1 if dt > 0 else -1)
    heads.setdefault(s, []).append([p, (px - cx) * t[0] + (py - cy) * t[1], turn, n, t, edge])
segs = []  # (ax, ay, bx, by, net)
def seg_dist(p, a, b):
    ax, ay = a; bx, by = b; dx, dy = bx - ax, by - ay; L2 = dx * dx + dy * dy
    u_ = 0 if L2 == 0 else max(0, min(1, ((p[0] - ax) * dx + (p[1] - ay) * dy) / L2))
    return math.hypot(p[0] - ax - u_ * dx, p[1] - ay - u_ * dy)
def free(x, y, net):
    for q in pads:
        if q['net'] == net: continue
        a, b, c_, e = box(q); m = C + W / 2
        if a - m < x < c_ + m and b - m < y < e + m: return False
    for s in segs:
        if s[4] != net and seg_dist((x, y), (s[0], s[1]), (s[2], s[3])) < W + C: return False
    return True
def plan_side(L, turns):
    n_ = len(L)
    if turns:
        best = None  # blocks [0,a) turn -1, [a,b) straight, [b,n) turn +1
        for a in range(n_ + 1):
            for b in range(a, n_ + 1):
                sc = sum((L[i][2] == -1) for i in range(a)) + sum((L[i][2] == 0) for i in range(a, b)) + sum((L[i][2] == 1) for i in range(b, n_))
                if best is None or sc > best[0]: best = (sc, a, b)
        _, a, b = best
    else: a, b = 0, n_
    out = []
    for i, h in enumerate(L):
        turn = -1 if i < a else (0 if i < b else 1)
        depth = B0 + STEP * i if turn == -1 else (B0 + STEP * (n_ - 1 - i) if turn == 1 else 0)
        out.append((0 if turn == 0 else 1, depth, int(h[0]['num']), h, turn, depth))
    out.sort(key=lambda o: o[:3])
    return out
def draw(order):
    made = []; total = 0.0
    for _, _, _, h, turn, depth in order:
        p, _, _, n, t, edge = h; net = p['net']; px, py = p['x'] / 1e6, p['y'] / 1e6
        sx, sy = (edge, py) if n[0] else (px, edge)
        pts = [(px, py), (sx, sy)]
        dirs = [(n, depth if turn else R)]
        if turn: dirs.append((((n[0] + turn * t[0]) / math.sqrt(2), (n[1] + turn * t[1]) / math.sqrt(2)), (R - depth) * math.sqrt(2)))
        x, y = sx, sy; ok = True
        for (ux, uy), L in dirs:
            k = 0; steps = int(L / 0.025)
            while k < steps:
                nx_, ny_ = x + ux * 0.025, y + uy * 0.025
                if not free(nx_, ny_, net): ok = False; break
                x, y = nx_, ny_; k += 1
            pts.append((x, y))
            if not ok: break
        if not ok and BACK > 0:  # stopped by an obstacle: end short of it, so the pin's route has room to leave
            (lx, ly) = pts[-2]; seg_len = math.hypot(x - lx, y - ly); cut = min(BACK, seg_len)
            if seg_len > 0: x, y = x - (x - lx) / seg_len * cut, y - (y - ly) / seg_len * cut
            pts[-1] = (x, y)
        L_ = sum(math.hypot(b_[0] - a_[0], b_[1] - a_[1]) for a_, b_ in zip(pts[1:], pts[2:]))
        if L_ < 0.4: continue
        total += L_
        for (ax, ay), (bx, by) in zip(pts, pts[1:]):
            if (ax, ay) != (bx, by): made.append((ax, ay, bx, by, net)); segs.append((ax, ay, bx, by, net))
    return made, total
MODE = os.environ.get('MODE', 'turn')  # turn | straight | space (per side, the longer of the two)
for s_ in sorted(heads):
    L = sorted(heads[s_], key=lambda h: h[1])
    if MODE == 'space':
        n0 = len(segs); _, lt = draw(plan_side(L, True)); del segs[n0:]
        _, ls = draw(plan_side(L, False)); del segs[n0:]
        pick = lt > ls + 1e-9
        print(s_, 'turn %.1f straight %.1f ->' % (lt, ls), 'turn' if pick else 'straight')
        draw(plan_side(L, pick))
    else: draw(plan_side(L, MODE == 'turn'))
add = [f'  (segment (start {a:.4f} {b:.4f}) (end {c:.4f} {e:.4f}) (width {W}) (layer F.Cu) (net {num[n]}))' for a, b, c, e, n in segs]
D = txt.rstrip(); open(out, 'w').write(D[:-1] + '\n'.join(add) + '\n)\n')
print(len({s[4] for s in segs}), 'heads', len(segs), 'segments')
