#!/usr/bin/env python3
"""Can the vias of a routed sketch be placed so that every cut around them holds at the real clearance?

Wires and pads keep their topology; each via may move anywhere inside its cell (it may not cross a pad-pad cut
segment). For each via and each visible neighbour feature P the sketch fixes the wires that pass between them, so
the via centre must keep dist(X, P) >= r_via + need(V, P) (+ r_P for a via neighbour), need = widths + clearances.

  stage 1 (pads only):   is the via's feasible region empty against the fixed pads alone?  -> proof: topology too tight
  stage 2 (all vias):    Gauss-Seidel placement of all vias; do the via-via constraints also close?

usage: viafit.py BOARD.kicad_pcb [--clearance 0.10]
"""
import argparse, math, sys
import numpy as np
sys.path.insert(0, __import__('os').path.dirname(__file__))
from tstar import load, chains, closest_pair, seg_hits_feature, crossings, need_terms

STEP, BOX = 0.004, 0.40


def rect_dist(X, Y, f):
    dx = np.maximum(np.abs(X - f['x']) - f['hw'], 0); dy = np.maximum(np.abs(Y - f['y']) - f['hh'], 0)
    return np.hypot(dx, dy)


def seg_cross(X, Y, vx, vy, p, q):
    """Does segment (vx,vy)-(X,Y) properly cross segment p-q? (vectorised over X, Y)"""
    def orient(ax, ay, bx, by, cx, cy):
        return (bx - ax) * (cy - ay) - (by - ay) * (cx - ax)
    o1 = orient(p[0], p[1], q[0], q[1], vx, vy); o2 = orient(p[0], p[1], q[0], q[1], X, Y)
    o3 = orient(vx, vy, X, Y, p[0], p[1]); o4 = orient(vx, vy, X, Y, q[0], q[1])
    return ((o1 > 0) != (o2 > 0)) & ((o3 > 0) != (o4 > 0))


def main():
    ap = argparse.ArgumentParser(); ap.add_argument('board'); ap.add_argument('--clearance', type=float, default=0.10)
    a = ap.parse_args(); c = a.clearance
    pads, vias, segs = load(a.board)
    ch = chains(segs)
    layers = sorted({s['layer'] for s in segs} | {L for v in vias for L in v['layers']})
    arr = {}
    for L in layers:
        lch = [k for k in ch if k['layer'] == L]
        A, B, cid, idx = [], [], [], []
        for k, cc in enumerate(lch):
            for j in range(len(cc['pts']) - 1):
                A.append(cc['pts'][j]); B.append(cc['pts'][j + 1]); cid.append(k); idx.append(j)
        arr[L] = (np.array(A), np.array(B), np.array(cid), np.array(idx), lch)

    def need(L, pa, pb, na, nb):
        A, B, cid, idx, lch = arr[L]
        obst = [p for p in pads if p['layer'] == L] + [v for v in vias if L in v['layers']]
        items = crossings(pa, pb, A, B, cid, idx, lch, obst); items.pop()
        ws, n = need_terms([(0, na, 0.0)] + items + [(1, nb, 0.0)])
        return ws + n * c, len(items)

    # constraints per via: (neighbour, layer, need)
    cons = []
    for vi, v in enumerate(vias):
        lst = []
        for L in v['layers']:
            obst = [p for p in pads if p['layer'] == L] + [w for w in vias if L in w['layers']]
            for f in obst:
                if f is v or abs(f['x'] - v['x']) > 1.5 or abs(f['y'] - v['y']) > 1.5:
                    continue
                pa, pb = closest_pair(v, f)
                if math.hypot(pb[0] - pa[0], pb[1] - pa[1]) > 1.2:
                    continue
                if any(seg_hits_feature(pa, pb, g) for g in obst if g is not v and g is not f):
                    continue
                nd, k = need(L, pa, pb, v['net'], f['net'])
                if nd == 0:
                    continue
                lst.append((f, L, nd))
        cons.append(lst)

    # pad-pad cut segments on each layer (cell walls a via may not cross)
    walls = {}
    for L in layers:
        lp = [p for p in pads if p['layer'] == L]; ws = []
        for i in range(len(lp)):
            for j in range(i + 1, len(lp)):
                pa, pb = closest_pair(lp[i], lp[j])
                if math.hypot(pb[0] - pa[0], pb[1] - pa[1]) < 1.0 and \
                        not any(seg_hits_feature(pa, pb, g) for g in lp if g is not lp[i] and g is not lp[j]):
                    ws.append((pa, pb))
        walls[L] = ws

    g = np.arange(-BOX, BOX + 1e-9, STEP)
    pos = [(v['x'], v['y']) for v in vias]

    def region(vi, with_vias):
        v = vias[vi]; X, Y = np.meshgrid(v['x'] + g, v['y'] + g)
        ok = np.ones_like(X, dtype=bool)
        for f, L, nd in cons[vi]:
            if f['kind'] == 'pad':
                ok &= rect_dist(X, Y, f) >= v['r'] + nd - 1e-9
            elif with_vias:
                wx, wy = pos[vias.index(f)]
                ok &= np.hypot(X - wx, Y - wy) >= v['r'] + f['r'] + nd - 1e-9
        for L in v['layers']:
            for p, q in walls.get(L, []):
                if abs((p[0] + q[0]) / 2 - v['x']) < 1.5 and abs((p[1] + q[1]) / 2 - v['y']) < 1.5:
                    ok &= ~seg_cross(X, Y, v['x'], v['y'], p, q)
        return X, Y, ok

    empty, moved = [], 0
    for vi in range(len(vias)):
        X, Y, ok = region(vi, False)
        if not ok.any():
            empty.append(vi)
    # stage 2: Gauss-Seidel over vias, each to the nearest point of its region given the others
    conflict = None
    for sweep in range(60):
        conflict = 0
        for vi in range(len(vias)):
            if vi in empty:
                continue
            X, Y, ok = region(vi, True)
            if not ok.any():
                conflict += 1; continue
            d2 = (X - pos[vi][0]) ** 2 + (Y - pos[vi][1]) ** 2; d2[~ok] = np.inf
            k = np.unravel_index(np.argmin(d2), d2.shape)
            if d2[k] > 1e-12:
                pos[vi] = (X[k], Y[k]); moved += 1
        if conflict == 0:
            break
    print(f"{a.board}: vias {len(vias)}, no room even against pads alone: {len(empty)}, "
          f"after placing all vias: {conflict} without room (sweeps {sweep + 1}, moves {moved})")
    for vi in empty[:6]:
        v = vias[vi]
        print(f"   no room: via {v['net']} at ({v['x']:.3f},{v['y']:.3f}); pad constraints:",
              [(round(f['x'], 2), round(f['y'], 2), round(nd, 3)) for f, L, nd in cons[vi] if f['kind'] == 'pad'])


if __name__ == '__main__':
    main()
