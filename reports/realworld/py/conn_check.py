import sys, pathlib
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import extract, kgeom
path, nets = sys.argv[1], sys.argv[2:]
b = kgeom.load(path)
ids = {n: i for i, n in b.nets.items()}
for n in nets:
    nl = extract.Netlist()
    extract.build_net(b, ids[n], nl, 4)
    parent = {}
    def f(x):
        parent.setdefault(x, x)
        while parent[x] != x:
            parent[x] = parent[parent[x]]; x = parent[x]
        return x
    for l in nl.lines:
        p = l.split()
        if p[0][0] in "RL":
            parent[f(p[1])] = f(p[2])
    pins = {k: v for k, v in nl.pin_nodes.items() if k[2] == n}
    comps = {}
    for k, v in pins.items():
        comps.setdefault(f(v), []).append(f"{k[0]}.{k[1]}")
    print(n, "components:", list(comps.values()))
    for part in b.parts:
        for p in part.pads:
            if p.netname == n and part.ref in ("U1",):
                print("   pad", part.ref, p.num, p.kind, p.shape, p.layers, "at", round(p.x, 3), round(p.y, 3))
    ends = [(round(t.x1, 3), round(t.y1, 3), t.layer) for t in b.tracks if t.net == ids[n]][:3] + [(round(t.x2, 3), round(t.y2, 3), t.layer) for t in b.tracks if t.net == ids[n]][:3]
    print("   some track ends", ends)
