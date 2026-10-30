# split_oracle.py UNR_GEO.json UNROUTED INJECTED OUT_HEADS OUT_LOCALS: split injected copper into clusters touching one pad (heads)
# and clusters joining two or more pads (local connections).
import json, re, sys
geo, unr, inj, oh, ol = sys.argv[1:6]
d = json.load(open(geo)); base = open(unr).read().splitlines(); lines = [l for l in open(inj).read().splitlines() if l not in set(base)]
seg = re.compile(r'\(segment \(start ([-\d.]+) ([-\d.]+)\) \(end ([-\d.]+) ([-\d.]+)\) \(width [\d.]+\) \(layer ([\w.]+)\) \(net (\d+)\)')
via = re.compile(r'\(via \(at ([-\d.]+) ([-\d.]+)\)[^)]*\)(?: \(drill [\d.]+\))? \(layers [\w.]+ [\w.]+\) \(net (\d+)\)')
par = list(range(len(lines))); find = lambda i: i if par[i] == i else find(par[i])
pts = []
for i, l in enumerate(lines):
    m = seg.search(l)
    if m: pts.append([(float(m.group(1)), float(m.group(2))), (float(m.group(3)), float(m.group(4)))]); continue
    m = via.search(l); pts.append([(float(m.group(1)), float(m.group(2)))])
key = {}
for i, ps in enumerate(pts):
    for p in ps:
        k = (round(p[0], 3), round(p[1], 3))
        if k in key: par[find(i)] = find(key[k])
        else: key[k] = i
def inpad(p, q):
    w, h = (q['h'], q['w']) if round(abs(q['angle'])) % 180 == 90 else (q['w'], q['h'])
    return abs(p[0] * 1e6 - q['x']) <= w / 2 and abs(p[1] * 1e6 - q['y']) <= h / 2
padsof = {}
for i, ps in enumerate(pts):
    for p in ps:
        for j, q in enumerate(d['pads']):
            if q['net'] and inpad(p, q): padsof.setdefault(find(i), set()).add(j)
H, Lc = [], []
for i, l in enumerate(lines):
    (H if len(padsof.get(find(i), ())) <= 1 else Lc).append(l)
for out, add in ((oh, H), (ol, Lc)):
    t = '\n'.join(base).rstrip(); open(out, 'w').write(t[:-1] + '\n'.join(add) + '\n)\n')
print(len(H), 'head lines', len(Lc), 'local lines')
