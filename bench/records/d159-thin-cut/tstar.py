#!/usr/bin/env python3
"""Cut-condition (Cole-Siegel / Leiserson-Maley) test of a routed board's topology.

For every pair of copper features (pads, vias) on a layer that see each other, the shortest segment between
them is a cut. The wires that must cross it (crossings of the routed centrelines, minus pairs of crossings of
one wire that enclose no feature and so can be pulled out) need sum(widths) + clearances between neighbours.
If any cut is short, the topology cannot be thickened to those rules, whatever the geometry does.

usage: tstar.py BOARD.kicad_pcb [--clearance 0.10] [--dmax 2.0] [--json OUT] [--pads-only]
"""
import argparse, json, math, sys
import numpy as np


def sexpr(text):
    toks, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c in ' \t\r\n':
            i += 1
        elif c in '()':
            toks.append(c); i += 1
        elif c == '"':
            j = i + 1; buf = []
            while text[j] != '"':
                if text[j] == '\\':
                    j += 1
                buf.append(text[j]); j += 1
            toks.append(('s', ''.join(buf))); i = j + 1
        else:
            j = i
            while j < n and text[j] not in ' \t\r\n()':
                j += 1
            toks.append(text[i:j]); i = j
    stack = [[]]
    for t in toks:
        if t == '(':
            stack.append([])
        elif t == ')':
            e = stack.pop(); stack[-1].append(e)
        else:
            stack[-1].append(t[1] if isinstance(t, tuple) else t)
    return stack[0][0]


def child(e, name):
    for c in e[1:]:
        if isinstance(c, list) and c and c[0] == name:
            return c
    return None


def children(e, name):
    return [c for c in e[1:] if isinstance(c, list) and c and c[0] == name]


def load(path):
    root = sexpr(open(path).read())
    pads, vias, segs = [], [], []
    for fp in children(root, 'footprint'):
        at = child(fp, 'at'); fx, fy = float(at[1]), float(at[2])
        frot = float(at[3]) if len(at) > 3 else 0.0
        assert frot == 0.0, 'rotated footprints not handled'
        for p in children(fp, 'pad'):
            assert p[3] == 'rect', p[3]
            pat = child(p, 'at'); assert len(pat) == 3 or float(pat[3]) % 180 == 0
            sz = child(p, 'size'); net = child(p, 'net')
            layers = child(p, 'layers')[1:]
            netname = (net[-1] if net else '') or f'<nc pad {len(pads)}>'
            for L in layers:
                if L.endswith('.Cu'):
                    pads.append(dict(kind='pad', x=fx + float(pat[1]), y=fy + float(pat[2]), hw=float(sz[1]) / 2,
                                     hh=float(sz[2]) / 2, net=netname, layer=L))
    for v in children(root, 'via'):
        at = child(v, 'at'); vias.append(dict(kind='via', x=float(at[1]), y=float(at[2]),
                                               r=float(child(v, 'size')[1]) / 2, net=child(v, 'net')[1],
                                               layers=child(v, 'layers')[1:]))
    for s in children(root, 'segment'):
        a, b = child(s, 'start'), child(s, 'end')
        segs.append(dict(x1=float(a[1]), y1=float(a[2]), x2=float(b[1]), y2=float(b[2]),
                         w=float(child(s, 'width')[1]), layer=child(s, 'layer')[1], net=child(s, 'net')[1]))
    assert not children(root, 'arc'), 'arcs not handled'
    return pads, vias, segs


# ---- geometry -------------------------------------------------------------------------------------------------

def closest_rect_point(f, px, py):
    return (min(max(px, f['x'] - f['hw']), f['x'] + f['hw']), min(max(py, f['y'] - f['hh']), f['y'] + f['hh']))


def closest_pair(a, b):
    """Closest points of two features (axis-aligned rects, circles)."""
    if a['kind'] == 'pad' and b['kind'] == 'pad':
        pa, pb = [0, 0], [0, 0]
        for k, (c, h) in enumerate((('x', 'hw'), ('y', 'hh'))):
            lo_a, hi_a = a[c] - a[h], a[c] + a[h]
            lo_b, hi_b = b[c] - b[h], b[c] + b[h]
            if hi_a < lo_b:
                pa[k], pb[k] = hi_a, lo_b
            elif hi_b < lo_a:
                pa[k], pb[k] = lo_a, hi_b
            else:
                m = (max(lo_a, lo_b) + min(hi_a, hi_b)) / 2; pa[k] = pb[k] = m
        return tuple(pa), tuple(pb)
    if a['kind'] == 'via' and b['kind'] == 'via':
        dx, dy = b['x'] - a['x'], b['y'] - a['y']; d = math.hypot(dx, dy) or 1e-12
        return (a['x'] + dx / d * a['r'], a['y'] + dy / d * a['r']), (b['x'] - dx / d * b['r'], b['y'] - dy / d * b['r'])
    flip = a['kind'] == 'via'
    r, v = (b, a) if flip else (a, b)
    q = closest_rect_point(r, v['x'], v['y'])
    dx, dy = q[0] - v['x'], q[1] - v['y']; d = math.hypot(dx, dy) or 1e-12
    pv = (v['x'] + dx / d * v['r'], v['y'] + dy / d * v['r'])
    return (pv, q) if flip else (q, pv)


def seg_hits_feature(p, q, f, eps=1e-6):
    """Does segment p-q pass through the interior of feature f (shrunk by eps)?"""
    if f['kind'] == 'via':
        return point_seg_dist(f['x'], f['y'], p, q) < f['r'] - eps
    # Liang-Barsky clip against the shrunk box
    x0, y0, dx, dy = p[0], p[1], q[0] - p[0], q[1] - p[1]
    t0, t1 = 0.0, 1.0
    for pk, qk in ((-dx, x0 - (f['x'] - f['hw'] + eps)), (dx, (f['x'] + f['hw'] - eps) - x0),
                   (-dy, y0 - (f['y'] - f['hh'] + eps)), (dy, (f['y'] + f['hh'] - eps) - y0)):
        if pk == 0:
            if qk < 0:
                return False
        else:
            t = qk / pk
            if pk < 0:
                t0 = max(t0, t)
            else:
                t1 = min(t1, t)
    return t0 < t1


def point_seg_dist(x, y, p, q):
    dx, dy = q[0] - p[0], q[1] - p[1]; L2 = dx * dx + dy * dy
    t = 0.0 if L2 == 0 else max(0.0, min(1.0, ((x - p[0]) * dx + (y - p[1]) * dy) / L2))
    return math.hypot(x - p[0] - t * dx, y - p[1] - t * dy)


def point_in_poly(x, y, poly):
    inside = False
    for i in range(len(poly)):
        (x1, y1), (x2, y2) = poly[i], poly[i - 1]
        if (y1 > y) != (y2 > y) and x < x1 + (y - y1) * (x2 - x1) / (y2 - y1):
            inside = not inside
    return inside


# ---- wires as chains -------------------------------------------------------------------------------------------

def chains(segs):
    """Split each net's segments on a layer into simple paths (ordered point lists)."""
    key = lambda x, y: (round(x * 1e4), round(y * 1e4))
    out = []
    by = {}
    for s in segs:
        by.setdefault((s['net'], s['layer']), []).append(s)
    for (net, layer), ss in sorted(by.items()):
        adj = {}
        for i, s in enumerate(ss):
            a, b = key(s['x1'], s['y1']), key(s['x2'], s['y2'])
            if a == b:
                continue
            adj.setdefault(a, []).append((b, i)); adj.setdefault(b, []).append((a, i))
        used = set()
        starts = [n for n in sorted(adj) if len(adj[n]) != 2] + sorted(adj)
        for st in starts:
            for nb, i in adj[st]:
                if i in used:
                    continue
                path, w, cur = [st], ss[i]['w'], st
                while True:
                    used.add(i); cur = nb; path.append(cur)
                    if len(adj[cur]) != 2:
                        break
                    nxt = [(n2, j) for n2, j in adj[cur] if j not in used]
                    if not nxt:
                        break
                    nb, i = nxt[0]
                out.append(dict(net=net, layer=layer, w=w, pts=[(x / 1e4, y / 1e4) for x, y in path]))
    return out


# ---- the test --------------------------------------------------------------------------------------------------

def analyse(path, clearance, dmax, pads_only):
    pads, vias, segs = load(path)
    layers = sorted({p['layer'] for p in pads} | {s['layer'] for s in segs} | {L for v in vias for L in v['layers']})
    ch = chains(segs)
    cuts, cancelled_total = [], 0
    for L in layers:
        feats = [p for p in pads if p['layer'] == L] + ([] if pads_only else [v for v in vias if L in v['layers']])
        obst = [p for p in pads if p['layer'] == L] + [v for v in vias if L in v['layers']]
        lch = [c for c in ch if c['layer'] == L]
        # flat segment arrays for vectorised crossing tests
        A, B, cid, idx = [], [], [], []
        for k, c in enumerate(lch):
            for j in range(len(c['pts']) - 1):
                A.append(c['pts'][j]); B.append(c['pts'][j + 1]); cid.append(k); idx.append(j)
        A, B, cid, idx = np.array(A), np.array(B), np.array(cid), np.array(idx)
        fx = np.array([f['x'] for f in feats]); fy = np.array([f['y'] for f in feats])
        for i in range(len(feats)):
            near = np.nonzero((np.abs(fx - fx[i]) < dmax + 1) & (np.abs(fy - fy[i]) < dmax + 1))[0]
            for j in near[near > i]:
                a, b = feats[i], feats[j]
                pa, pb = closest_pair(a, b)
                d = math.hypot(pb[0] - pa[0], pb[1] - pa[1])
                if d > dmax or d < 1e-9:
                    continue
                mx, my = (pa[0] + pb[0]) / 2, (pa[1] + pb[1]) / 2
                if any(seg_hits_feature(pa, pb, f) for f in obst if f is not a and f is not b
                       and abs(f['x'] - mx) < d + 1 and abs(f['y'] - my) < d + 1):
                    continue  # not visible
                items = crossings(pa, pb, A, B, cid, idx, lch, obst)
                cancelled_total += items.pop()
                seq = [(0.0, a['net'], 0.0)] + items + [(1.0, b['net'], 0.0)]
                if len(seq) == 2 and a['net'] == b['net']:
                    continue
                wsum, nclr = need_terms(seq)
                if nclr == 0:
                    continue
                need = wsum + nclr * clearance
                cuts.append(dict(layer=L, a=desc(a), b=desc(b), pa=pa, pb=pb, d=d, k=len(items),
                                 nets=[n for _, n, _ in items], need=need, slack=d - need,
                                 s=(d - wsum) / (nclr * clearance)))
    return cuts, cancelled_total


def need_terms(seq):
    """Widths and clearance gaps along a cut: [end a] + wires + [end b] as (u, net, width).
    Copper of one net may overlap: wires of an end's net next to that end, and runs of one net, merge."""
    s = list(seq)
    while len(s) > 2 and s[1][1] == s[0][1]:
        s.pop(1)
    while len(s) > 2 and s[-2][1] == s[-1][1]:
        s.pop(-2)
    runs = []
    for item in s:
        if runs and runs[-1][1] == item[1]:
            runs[-1] = (runs[-1][0], item[1], max(runs[-1][2], item[2]))
        else:
            runs.append(item)
    return sum(w for _, _, w in runs), len(runs) - 1


def desc(f):
    return dict(kind=f['kind'], x=f['x'], y=f['y'], net=f['net'])


def crossings(pa, pb, A, B, cid, idx, lch, obst):
    """Wires crossing the cut pa-pb: list of (u along cut, net, width), last element the number cancelled."""
    ex, ey = pb[0] - pa[0], pb[1] - pa[1]
    sa = (A[:, 0] - pa[0]) * ey - (A[:, 1] - pa[1]) * ex
    sb = (B[:, 0] - pa[0]) * ey - (B[:, 1] - pa[1]) * ex
    sa = np.where(sa == 0, 1e-15, sa); sb = np.where(sb == 0, 1e-15, sb)  # symbolic perturbation
    hit = np.nonzero((sa > 0) != (sb > 0))[0]
    per = {}
    L2 = ex * ex + ey * ey
    for h in hit:
        t = sa[h] / (sa[h] - sb[h])
        x, y = A[h, 0] + t * (B[h, 0] - A[h, 0]), A[h, 1] + t * (B[h, 1] - A[h, 1])
        u = ((x - pa[0]) * ex + (y - pa[1]) * ey) / L2
        if 0 < u < 1:
            per.setdefault(int(cid[h]), []).append((int(idx[h]) + t, u, 1 if sb[h] > 0 else -1, (x, y)))
    out, cancelled = [], 0
    for k, xs in per.items():
        xs.sort()
        stack = []
        pts = lch[k]['pts']
        for x in xs:
            if stack and stack[-1][2] == -x[2] and empty_loop(stack[-1], x, pts, pa, pb, obst):
                stack.pop(); cancelled += 2
            else:
                stack.append(x)
        out += [(u, lch[k]['net'], lch[k]['w']) for _, u, _, _ in stack]
    out.sort()
    out.append(cancelled)
    return out


def empty_loop(c1, c2, pts, pa, pb, obst):
    i1, i2 = int(c1[0]), int(c2[0])
    poly = [c1[3]] + pts[i1 + 1:i2 + 1] + [c2[3]]
    xs = [p[0] for p in poly]; ys = [p[1] for p in poly]
    for f in obst:
        if min(xs) <= f['x'] <= max(xs) and min(ys) <= f['y'] <= max(ys) and point_in_poly(f['x'], f['y'], poly):
            return False
    return True


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('board'); ap.add_argument('--clearance', type=float, default=0.10)
    ap.add_argument('--dmax', type=float, default=2.0); ap.add_argument('--json')
    ap.add_argument('--pads-only', action='store_true', help='cuts between pads only (vias may move)')
    a = ap.parse_args()
    cuts, canc = analyse(a.board, a.clearance, a.dmax, a.pads_only)
    bad = sorted([c for c in cuts if c['slack'] < -1e-4], key=lambda c: c['slack'])
    smin = min(c['s'] for c in cuts)
    kinds = {}
    for c in bad:
        kk = '-'.join(sorted((c['a']['kind'], c['b']['kind'])))
        kinds[kk] = kinds.get(kk, 0) + 1
    print(f"{a.board}: cuts {len(cuts)}, crossings cancelled {canc}, over-full at c={a.clearance}: {len(bad)} {kinds}, "
          f"s* (clearance scale) = {smin:.3f}")
    for c in bad[:8]:
        print(f"  {c['layer']} {c['a']['kind']}({c['a']['x']:.2f},{c['a']['y']:.2f}) - {c['b']['kind']}({c['b']['x']:.2f},"
              f"{c['b']['y']:.2f}) d={c['d']:.3f} wires={c['k']} need={c['need']:.3f} short={-c['slack']:.3f}")
    if a.json:
        json.dump(dict(board=a.board, clearance=a.clearance, cuts=len(cuts), cancelled=canc, s_star=smin, bad=bad),
                  open(a.json, 'w'), indent=1)


if __name__ == '__main__':
    main()
