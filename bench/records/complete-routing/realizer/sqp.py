#!/usr/bin/env python3
"""SQP realizer prototype without an LP solver (doc 20 §2.3). Each outer iteration linearises the constraints near the current positions
and solves the elastic QP

    min 1/2 |dx|^2 + M sum s   s.t.   g_i + a_i.dx + s_i >= 0,  s >= 0,  lo <= x + dx <= hi,  |dx| <= rho

by dual coordinate ascent (Hildreth 1957): one multiplier per row in [0, M]; bounds are kept by clipping the primal after each pass,
which is exact for box constraints in the separable case and close enough for a prototype. Deterministic: fixed row order.

usage: sqp.py DUMP.json [--box MM] [--iters N] [--passes P]
"""
import argparse, sys
import numpy as np
sys.path.insert(0, __file__.rsplit("/", 1)[0])
import realize


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dump")
    ap.add_argument("--box", type=float, default=0)
    ap.add_argument("--iters", type=int, default=20)
    ap.add_argument("--passes", type=int, default=200)
    ap.add_argument("--rho", type=float, default=0.05)
    ap.add_argument("--near", type=float, default=0.05)
    ap.add_argument("--M", type=float, default=1.0)
    a = ap.parse_args()
    d, N, pads, cons = realize.load(a.dump)
    margin = d["margin"]
    x0, y0 = N[:, 0].copy(), N[:, 1].copy()
    x, y = N[:, 2].copy(), N[:, 3].copy()
    fixed = N[:, 8] > 0
    if a.box > 0:
        lox, hix, loy, hiy = x0 - a.box, x0 + a.box, y0 - a.box, y0 + a.box
        inpad = N[:, 9] >= 0
        lox[inpad], hix[inpad], loy[inpad], hiy[inpad] = N[inpad, 4], N[inpad, 5], N[inpad, 6], N[inpad, 7]
    else:
        lox, hix, loy, hiy = N[:, 4], N[:, 5], N[:, 6], N[:, 7]
    print(f"start too close {realize.status(cons, x, y, pads, margin)}")
    rho = a.rho
    for it in range(a.iters):
        rows = []
        for c in cons:
            r = realize.evaluate(c, x, y, pads)
            if r is None:
                continue
            g = r[0] - c[6]
            if g >= a.near:
                continue
            terms = [(n, gx, gy) for n, gx, gy in r[1] if not fixed[n] and (gx or gy)]
            if terms:
                rows.append((g, terms))
        if not rows:
            break
        dx, dy = {}, {}
        lam = [0.0] * len(rows)
        lo = {n: (max(-rho, lox[n] - x[n]), min(rho, hix[n] - x[n]), max(-rho, loy[n] - y[n]), min(rho, hiy[n] - y[n]))
              for _, t in rows for n, _, _ in t}
        for n in lo:
            dx[n] = dy[n] = 0.0
        for _ in range(a.passes):
            change = 0.0
            for k, (g, terms) in enumerate(rows):
                # row: a.d >= -g ; dual step on lam_k with dx = sum lam a (unclipped), then clip to the box
                ad = sum(gx * dx[n] + gy * dy[n] for n, gx, gy in terms)
                aa = sum(gx * gx + gy * gy for n, gx, gy in terms)
                if aa == 0:
                    continue
                step = (-g - ad) / aa
                new = min(a.M, max(0.0, lam[k] + step))
                dl = new - lam[k]
                if dl == 0:
                    continue
                lam[k] = new
                change = max(change, abs(dl))
                for n, gx, gy in terms:
                    l = lo[n]
                    dx[n] = min(l[1], max(l[0], dx[n] + dl * gx))
                    dy[n] = min(l[3], max(l[2], dy[n] + dl * gy))
            if change < 1e-9:
                break
        xn, yn = x.copy(), y.copy()
        for n in dx:
            xn[n] += dx[n]
            yn[n] += dy[n]
        before = realize.status(cons, x, y, pads, margin)
        after = realize.status(cons, xn, yn, pads, margin)
        print(f"iter {it}: rho {rho:.4f}, {len(rows)} rows; too close {before} -> {after}")
        if after[0] < before[0] or (after[0] == before[0] and after[1] < before[1]):
            x, y = xn, yn
            if after[0] == 0:
                break
        else:
            rho *= 0.5
            if rho < 1e-4:
                break
    mv = np.hypot(x - x0, y - y0)
    realize.explain(a.dump, x, y)
    print(f"end: too close {realize.status(cons, x, y, pads, margin)}; moved {int((mv > 1e-6).sum())} nodes, at most {mv.max():.4f} mm")


if __name__ == "__main__":
    main()
