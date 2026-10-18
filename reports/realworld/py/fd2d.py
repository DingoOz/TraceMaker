# SPDX-License-Identifier: GPL-3.0-or-later
"""Two-dimensional electrostatic solver for the cross-section of coupled PCB traces: per-unit-length capacitance (and, from the air-filled
case, inductance) matrices by finite differences. Conductors are rectangles at fixed potential, dielectrics are slabs, the plane(s) are at 0 V.

  cap_matrix(conductors, slabs, planes, ...) -> C (F/m), Maxwell matrix, conductors only, planes are the reference
  inductance_matrix(...) -> L (H/m) = mu0 eps0 inv(C with every dielectric replaced by air), quasi-TEM

Units: mm for geometry. Validation: a single microstrip against Hammerstad and Jensen (tests in validate_fd2d.py).
"""
from __future__ import annotations

import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spl

EPS0 = 8.8541878128e-12
MU0 = 4e-7 * np.pi


def _solve(conductors, slabs, planes, xmin, xmax, ytop, dx, air_only=False):
    nx = int(round((xmax - xmin) / dx))
    ny = int(round(ytop / dx))
    eps = np.ones((ny, nx))
    ys = (np.arange(ny) + 0.5) * dx
    xs = xmin + (np.arange(nx) + 0.5) * dx
    if not air_only:
        for (y0, y1, er) in slabs:
            m = (ys >= y0) & (ys < y1)
            eps[m, :] = er
    fixed = np.full((ny, nx), -1, dtype=int)  # -1 free, -2 plane (0 V), k conductor index
    for (y0, y1) in planes:
        m = (ys >= y0) & (ys < y1)
        fixed[m, :] = -2
    fixed[-1, :] = -2  # far boundary
    for k, (x0, x1, y0, y1) in enumerate(conductors):
        mx = (xs >= x0) & (xs < x1)
        my = (ys >= y0) & (ys < y1)
        fixed[np.ix_(my, mx)] = k
    # unknowns: free cells; Neumann on the sides, Dirichlet cells carry their potential
    idx = -np.ones((ny, nx), dtype=int)
    free = fixed == -1
    idx[free] = np.arange(free.sum())
    n = int(free.sum())

    def face(e1, e2):
        return 2 * e1 * e2 / (e1 + e2)

    rows, cols, vals = [], [], []
    diag = np.zeros(n)
    nbrs = []  # (free index, other cell (r,c), eps face) for the right-hand side per excitation
    rhs_terms = []
    for (dr, dc) in ((0, 1), (0, -1), (1, 0), (-1, 0)):
        r0, r1 = max(0, -dr), ny - max(0, dr)
        c0, c1 = max(0, -dc), nx - max(0, dc)
        R, C = np.meshgrid(np.arange(r0, r1), np.arange(c0, c1), indexing="ij")
        R, C = R.ravel(), C.ravel()
        a = free[R, C]
        R, C = R[a], C[a]
        R2, C2 = R + dr, C + dc
        w = face(eps[R, C], eps[R2, C2])
        i = idx[R, C]
        diag_add = np.zeros(n)
        np.add.at(diag, i, w)
        other_free = free[R2, C2]
        rows.append(i[other_free])
        cols.append(idx[R2, C2][other_free])
        vals.append(-w[other_free])
        fx = ~other_free
        rhs_terms.append((i[fx], fixed[R2, C2][fx], w[fx], R[fx], C[fx], R2[fx], C2[fx]))
    A = sp.coo_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))), shape=(n, n)).tocsc() + sp.diags(diag)
    lu = spl.splu(A.tocsc())
    nc = len(conductors)
    Cm = np.zeros((nc, nc))
    for j in range(nc):
        b = np.zeros(n)
        for (i, f, w, *_rest) in rhs_terms:
            m = f == j
            np.add.at(b, i[m], w[m] * 1.0)
        phi = lu.solve(b)
        full = np.zeros((ny, nx))
        full[free] = phi
        for k in range(nc):
            full[fixed == k] = 1.0 if k == j else 0.0
        # charge on conductor k: sum over faces to non-conductor neighbours of eps_face * (V_k - phi_nb)
        for (dr, dc) in ((0, 1), (0, -1), (1, 0), (-1, 0)):
            r0, r1 = max(0, -dr), ny - max(0, dr)
            c0, c1 = max(0, -dc), nx - max(0, dc)
            R, C = np.meshgrid(np.arange(r0, r1), np.arange(c0, c1), indexing="ij")
            R, C = R.ravel(), C.ravel()
            kk = fixed[R, C]
            a = kk >= 0
            R, C, kk = R[a], C[a], kk[a]
            R2, C2 = R + dr, C + dc
            nb = fixed[R2, C2]
            ok = (nb != kk) | (nb < 0)
            ok &= ~((nb >= 0) & (nb != kk))  # a neighbour conductor cell is another conductor: its potential is explicit too
            # include conductor-conductor faces (different conductors touching) for completeness
            w = face(eps[R, C], eps[R2, C2])
            q = w * (full[R, C] - full[R2, C2])
            for k in range(nc):
                sel = (kk == k) & ((nb != k))
                Cm[k, j] += q[sel].sum()
    return Cm * EPS0


def cap_matrix(conductors, slabs, planes, dx=0.01, margin=1.2, ytop=None):
    xmin = min(c[0] for c in conductors) - margin
    xmax = max(c[1] for c in conductors) + margin
    ytop = ytop or (max(c[3] for c in conductors) + 2.5)
    return _solve(conductors, slabs, planes, xmin, xmax, ytop, dx)


def inductance_matrix(conductors, slabs, planes, dx=0.01, margin=1.2, ytop=None):
    xmin = min(c[0] for c in conductors) - margin
    xmax = max(c[1] for c in conductors) + margin
    ytop = ytop or (max(c[3] for c in conductors) + 2.5)
    C0 = _solve(conductors, slabs, planes, xmin, xmax, ytop, dx, air_only=True)
    return MU0 * EPS0 * np.linalg.inv(C0)
