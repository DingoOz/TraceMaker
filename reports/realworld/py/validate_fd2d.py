# SPDX-License-Identifier: GPL-3.0-or-later
import math, sys, pathlib, time
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import numpy as np
import fd2d

def hammerstad_z0(w, h, er):
    u = w / h
    ee = (er + 1) / 2 + (er - 1) / 2 / math.sqrt(1 + 12 / u)
    if u <= 1:
        z = 60 / math.sqrt(ee) * math.log(8 / u + u / 4)
    else:
        z = 120 * math.pi / (math.sqrt(ee) * (u + 1.393 + 0.667 * math.log(u + 1.444)))
    return z, ee

c = 299792458.0
t0 = time.time()
for w, h, er in ((0.3, 0.2104, 4.5), (0.15, 0.2104, 4.5), (0.25, 1.6, 4.6)):
    cond = [(-w / 2, w / 2, h, h + 0.035)]
    slabs = [(0.0, h, er)]
    C = fd2d.cap_matrix(cond, slabs, [(0.0, 0.01)], dx=0.01)[0, 0]
    C0 = fd2d.cap_matrix(cond, [], [(0.0, 0.01)], dx=0.01)[0, 0]
    z = 1 / (c * math.sqrt(C * C0))
    ee = C / C0
    zh, eeh = hammerstad_z0(w, h, er)
    print(f"w={w} h={h} er={er}: solver Z0 {z:.1f} ohm eeff {ee:.2f} | Hammerstad Z0 {zh:.1f} eeff {eeh:.2f} (strip thickness 0.035 lowers Z0 a few %)")
print("time", round(time.time() - t0, 1), "s")
# coupled pair: symmetric matrix, positive diagonal, negative off-diagonal
w, h, er, s = 0.15, 0.2104, 4.5, 0.127
cond = [(-w - s / 2, -s / 2, h, h + 0.035), (s / 2, s / 2 + w, h, h + 0.035)]
C = fd2d.cap_matrix(cond, [(0.0, h, er)], [(0.0, 0.01)], dx=0.01)
print("coupled C (pF/m):\n", np.round(C * 1e12, 2))
print("symmetry error", abs(C[0, 1] - C[1, 0]) / abs(C[0, 1]))
