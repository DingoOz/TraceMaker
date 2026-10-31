#!/usr/bin/env python3
"""Penalty realizer prototype (doc 20 §2.3): minimise  sum max(0, req - d)^2 + mu * |x - x_start|^2  over the movable nodes within their
bounds (L-BFGS-B), with the legaliser's constraints. No LP solver needed, so it ports to C++ as a plain gradient method.

usage: penalty.py DUMP.json [--box MM] [--mu W] [--target T]
"""
import argparse, sys
import numpy as np
from scipy.optimize import minimize
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import realize


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--box", type=float, default=0)
    ap.add_argument("--mu", type=float, default=1e-4)
    ap.add_argument("--target", type=float, default=0.5, help="aim at req - target * margin (0: req, 1: the violation threshold)")
    ap.add_argument("--near", type=float, default=0.3)
    ap.add_argument("--outer", type=int, default=1, help="augmented Lagrangian rounds (1 = plain penalty)")
    a = ap.parse_args()
    d, N, pads, cons = realize.load(a.dump)
    margin = d["margin"]
    x0, y0 = N[:, 0].copy(), N[:, 1].copy()
    xs, ys = N[:, 2].copy(), N[:, 3].copy()
    fixed = N[:, 8] > 0
    if a.box > 0:
        lox, hix, loy, hiy = x0 - a.box, x0 + a.box, y0 - a.box, y0 + a.box
        inpad = N[:, 9] >= 0
        lox[inpad], hix[inpad], loy[inpad], hiy[inpad] = N[inpad, 4], N[inpad, 5], N[inpad, 6], N[inpad, 7]
    else:
        lox, hix, loy, hiy = N[:, 4], N[:, 5], N[:, 6], N[:, 7]
    # only constraints that are near at the start take part (as the legaliser's candidate pairs do)
    act = []
    for c in cons:
        r = realize.evaluate(c, xs, ys, pads)
        if r is not None and r[0] - c[6] < a.near:
            act.append(c)
    mov = [i for i in range(len(xs)) if not fixed[i]]
    idx = {n: k for k, n in enumerate(mov)}
    print(f"{len(act)} constraints near, {len(mov)} movable nodes; start too close {realize.status(cons, xs, ys, pads, margin)}")

    def unpack(v):
        x, y = xs.copy(), ys.copy()
        x[mov], y[mov] = v[: len(mov)], v[len(mov):]
        return x, y

    lam = np.zeros(len(act))
    rho = 1.0

    def misses(v):
        x, y = unpack(v)
        out = []
        for c in act:
            r = realize.evaluate(c, x, y, pads)
            out.append(None if r is None else (c[6] - a.target * margin - r[0], r[1]))
        return out

    def f(v):
        g = np.zeros_like(v)
        val = 0.0
        for k, m in enumerate(misses(v)):
            if m is None:
                continue
            miss, grad = m
            h = miss + lam[k] / rho  # augmented Lagrangian term (rho/2) max(0, miss + lam/rho)^2
            if h <= 0:
                continue
            val += rho / 2 * h * h
            for n, gx, gy in grad:
                if n in idx:
                    g[idx[n]] -= rho * h * gx
                    g[len(mov) + idx[n]] -= rho * h * gy
        dx, dy = v[: len(mov)] - xs[mov], v[len(mov):] - ys[mov]
        val += a.mu * (dx @ dx + dy @ dy)
        g[: len(mov)] += 2 * a.mu * dx
        g[len(mov):] += 2 * a.mu * dy
        return val * 1e4, g * 1e4

    v0 = np.concatenate([xs[mov], ys[mov]])
    bounds = [(lox[n], hix[n]) for n in mov] + [(loy[n], hiy[n]) for n in mov]
    for outer in range(a.outer):
        res = minimize(f, v0, jac=True, method="L-BFGS-B", bounds=bounds, options={"maxiter": 3000, "maxfun": 6000})
        v0 = res.x
        for k, m in enumerate(misses(v0)):
            if m is not None:
                lam[k] = max(0.0, lam[k] + rho * m[0])
        x, y = unpack(v0)
        print(f"outer {outer}: rho {rho:g}, too close {realize.status(cons, x, y, pads, margin)}")
        rho *= 4
    x, y = unpack(v0)
    mv = np.hypot(x - x0, y - y0)
    print(f"L-BFGS-B: {res.nit} iterations, {res.message}")
    realize.explain(a.dump, x, y)
    print(f"end: too close {realize.status(cons, x, y, pads, margin)}; moved {int((mv > 1e-6).sum())} nodes, at most {mv.max():.4f} mm")


if __name__ == "__main__":
    main()
