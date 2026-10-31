#!/usr/bin/env python3
"""Realizer prototype (doc 20 §2.3): sequential linear programming with a trust region on a legaliser dump (TM_LEGALIZE_DUMP).

usage: realize.py DUMP.json [--from start|now] [--box MM] [--iters N] [--rho MM]

Constraints are the legaliser's own (route/legalize.cpp): a node's distance to a segment, a node, a pad, or a pad corner's distance
to a segment must reach req (req includes the margin; a pair is "too close" below req - margin). Each iteration linearises the
constraints near the current positions and solves  min sum|dx| + M * sum(slack)  subject to  g + grad.dx + slack >= 0,
|dx| <= rho, lo <= x + dx <= hi  (an elastic LP, so it always has a solution and the slack shows what cannot be met).
"""
import argparse, json, math, sys
import numpy as np
from scipy.optimize import linprog
from scipy.sparse import coo_matrix


def load(path):
    d = json.load(open(path))
    N = np.array(d["nodes"], dtype=float)
    return d, N, np.array(d["pads"], dtype=float), d["cons"]


def evaluate(c, x, y, pads):
    """distance, [(node, gx, gy)] gradient of the distance; None when degenerate."""
    kind, a, b, cc, qx, qy, req = c
    if kind == 0:  # PointSeg: node a against segment b-c
        p, s, e = a, b, cc
        dx, dy = x[e] - x[s], y[e] - y[s]
        l = dx * dx + dy * dy
        t = 0.0 if l == 0 else min(1.0, max(0.0, ((x[p] - x[s]) * dx + (y[p] - y[s]) * dy) / l))
        fx, fy = x[s] + t * dx, y[s] + t * dy
        ex, ey = x[p] - fx, y[p] - fy
        dd = math.hypot(ex, ey)
        if dd < 1e-9:
            return None
        nx, ny = ex / dd, ey / dd
        return dd, [(p, nx, ny), (s, -(1 - t) * nx, -(1 - t) * ny), (e, -t * nx, -t * ny)]
    if kind == 1:  # PointPoint
        ex, ey = x[a] - x[b], y[a] - y[b]
        dd = math.hypot(ex, ey)
        if dd < 1e-9:
            return None
        nx, ny = ex / dd, ey / dd
        return dd, [(a, nx, ny), (b, -nx, -ny)]
    if kind == 2:  # PointPad
        cx, cy, hx, hy, rr, ca, sa, circle = pads[cc]
        if circle:
            ex, ey = x[a] - cx, y[a] - cy
            dd = math.hypot(ex, ey)
            if dd < 1e-9:
                return None
            return dd - hx, [(a, ex / dd, ey / dd)]
        dx, dy = x[a] - cx, y[a] - cy
        lx, ly = ca * dx + sa * dy, -sa * dx + ca * dy
        fx, fy = lx - min(hx, max(-hx, lx)), ly - min(hy, max(-hy, ly))
        ex, ey = ca * fx - sa * fy, sa * fx + ca * fy
        dd = math.hypot(ex, ey)
        if dd < 1e-9:
            return None
        return dd - rr, [(a, ex / dd, ey / dd)]
    # CornerSeg: fixed corner (qx, qy) against segment b-c
    s, e = b, cc
    dx, dy = x[e] - x[s], y[e] - y[s]
    l = dx * dx + dy * dy
    t = 0.0 if l == 0 else min(1.0, max(0.0, ((qx - x[s]) * dx + (qy - y[s]) * dy) / l))
    fx, fy = x[s] + t * dx, y[s] + t * dy
    ex, ey = fx - qx, fy - qy
    dd = math.hypot(ex, ey)
    if dd < 1e-9:
        return None
    nx, ny = ex / dd, ey / dd
    return dd, [(s, (1 - t) * nx, (1 - t) * ny), (e, t * nx, t * ny)]


def status(cons, x, y, pads, margin):
    bad, worst = 0, 0.0
    for c in cons:
        r = evaluate(c, x, y, pads)
        if r and r[0] < c[6] - margin - 1e-9:
            bad += 1
            worst = max(worst, c[6] - margin - r[0])
    return bad, worst


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--from", dest="start", default="now", choices=["start", "now"])
    ap.add_argument("--box", type=float, default=0, help="move box (mm) around the start positions; 0 = the dump's bounds")
    ap.add_argument("--iters", type=int, default=30)
    ap.add_argument("--rho", type=float, default=0.05)
    ap.add_argument("--near", type=float, default=0.05, help="constraints within this of their target are linearised")
    a = ap.parse_args()
    d, N, pads, cons = load(a.dump)
    margin = d["margin"]
    x0, y0 = N[:, 0].copy(), N[:, 1].copy()
    x, y = (N[:, 2].copy(), N[:, 3].copy()) if a.start == "now" else (x0.copy(), y0.copy())
    fixed = N[:, 8] > 0
    if a.box > 0:
        lox, hix, loy, hiy = x0 - a.box, x0 + a.box, y0 - a.box, y0 + a.box
        # a node in its own pad keeps the dump's pad bounds
        inpad = N[:, 9] >= 0
        lox[inpad], hix[inpad], loy[inpad], hiy[inpad] = N[inpad, 4], N[inpad, 5], N[inpad, 6], N[inpad, 7]
    else:
        lox, hix, loy, hiy = N[:, 4], N[:, 5], N[:, 6], N[:, 7]
    print(f"{a.dump}: {len(x)} nodes, {len(cons)} constraints; start '{a.start}': too close {status(cons, x, y, pads, margin)}")
    rho = a.rho
    M = 1000.0
    for it in range(a.iters):
        rows, var_of = [], {}
        for c in cons:
            r = evaluate(c, x, y, pads)
            if r is None:
                continue
            g = r[0] - c[6]
            if g >= a.near:
                continue
            terms = [(n, gx, gy) for n, gx, gy in r[1] if not fixed[n] and (gx or gy)]
            if not terms:
                continue
            for n, _, _ in terms:
                var_of.setdefault(n, len(var_of))
            rows.append((g, terms))
        if not rows:
            print("no constraint near: done")
            break
        nv = len(var_of)
        # variables: dx (nv), dy (nv) as free bounded; |d| via u >= d, u >= -d  (u: 2nv); slack per row (nr)
        nr = len(rows)
        n_d, n_u, n_s = 2 * nv, 2 * nv, nr
        nvar = n_d + n_u + n_s
        cost = np.concatenate([np.zeros(n_d), np.ones(n_u), M * np.ones(n_s)])
        ri, ci, vv, b = [], [], [], []
        k = 0
        for i, (g, terms) in enumerate(rows):  # -(grad.d) - s <= g
            for n, gx, gy in terms:
                j = var_of[n]
                ri += [k, k]
                ci += [j, nv + j]
                vv += [-gx, -gy]
            ri.append(k), ci.append(n_d + n_u + i), vv.append(-1.0)
            b.append(g)
            k += 1
        for j in range(n_d):  # d - u <= 0, -d - u <= 0
            ri += [k, k]; ci += [j, n_d + j]; vv += [1.0, -1.0]; b.append(0.0); k += 1
            ri += [k, k]; ci += [j, n_d + j]; vv += [-1.0, -1.0]; b.append(0.0); k += 1
        A = coo_matrix((vv, (ri, ci)), shape=(k, nvar)).tocsr()
        bounds = [None] * nvar
        nodes = sorted(var_of, key=var_of.get)
        for n in nodes:
            j = var_of[n]
            bounds[j] = (max(-rho, lox[n] - x[n]), min(rho, hix[n] - x[n]))
            bounds[nv + j] = (max(-rho, loy[n] - y[n]), min(rho, hiy[n] - y[n]))
        for j in range(n_d, nvar):
            bounds[j] = (0, None)
        res = linprog(cost, A_ub=A, b_ub=np.array(b), bounds=bounds, method="highs")
        if res.status != 0:
            print(f"iter {it}: LP status {res.status} {res.message}")
            break
        sol = res.x
        xn, yn = x.copy(), y.copy()
        for n in nodes:
            j = var_of[n]
            xn[n] += sol[j]
            yn[n] += sol[nv + j]
        before = status(cons, x, y, pads, margin)
        after = status(cons, xn, yn, pads, margin)
        slack = sol[n_d + n_u:].sum()
        print(f"iter {it}: rho {rho:.4f}, {nr} rows, {nv} nodes, LP slack {slack:.5f}; too close {before} -> {after}")
        if after[0] <= before[0] or after[1] < before[1]:
            x, y = xn, yn
            if after[0] == 0:
                break
            if slack < 1e-7:
                rho = min(rho * 1.5, 0.25)
        else:
            rho *= 0.5
            if rho < 1e-4:
                break
    mv = np.hypot(x - x0, y - y0)
    explain(a.dump, x, y)
    print(f"end: too close {status(cons, x, y, pads, margin)}; moved {int((mv > 1e-6).sum())} nodes, at most {mv.max():.4f} mm from the start")



def explain(path, x, y):
    d, N, pads, cons = load(path)
    margin = d["margin"]
    for c in cons:
        r = evaluate(c, x, y, pads)
        if r and r[0] < c[6] - margin - 1e-9:
            nodes = [n for n, _, _ in r[1]]
            info = []
            for n in nodes:
                lo_hit = abs(x[n] - N[n, 4]) < 1e-6 or abs(x[n] - N[n, 5]) < 1e-6 or abs(y[n] - N[n, 6]) < 1e-6 or abs(y[n] - N[n, 7]) < 1e-6
                info.append(f"n{n}{'F' if N[n, 8] else ''}{'P' + str(int(N[n, 9])) if N[n, 9] >= 0 else ''}{'B' if lo_hit else ''}")
            print(f"  kind {c[0]} {r[0]:.4f} of {c[6] - margin:.4f}  nodes {' '.join(info)}  pad {c[3] if c[0] == 2 else ''} at ({x[nodes[0]]:.3f},{y[nodes[0]]:.3f})")


if __name__ == "__main__":
    main()
