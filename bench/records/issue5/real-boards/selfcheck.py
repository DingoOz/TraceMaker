"""Every library footprint of a board (2 pads or more) gets a reference derived from its FIRST instance; the whole board is checked against them.
All instances of one library footprint should agree with the first unless somebody edited a copy: the check must find those, and only those.
usage: selfcheck.py <tracemaker> <board> <outdir>"""
import collections, json, pathlib, subprocess, sys
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import sx
tm, board, outdir = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
outdir.mkdir(parents=True, exist_ok=True)
fps = sx.footprints(board)
by = collections.OrderedDict()
for f in fps:
    by.setdefault(f['lib'], []).append(f)
refs, chosen = [], []
for lib, l in by.items():
    front = [f for f in l if not f['back']]
    if not front:
        continue  # a footprint that is only on the back has its pads stored mirrored: it is not the pattern as drawn
    src = front[0]
    pads = [p for p in src['pads'] if p['type'] == 'np_thru_hole' or any(x.endswith('.Cu') for x in p['layers'])]
    if len(pads) < 2 or any(p['w'] <= 0 for p in pads):
        continue
    chosen.append(lib)
    refs.append({'brand': 'Scratch', 'part': lib, 'datasheet': {'document': 'first front instance ' + src['ref']}, 'match': {'lib_id': [lib.replace('[', '?').replace(']', '?')]},
                 'pads': [{'number': p['number'], 'type': p['type'] if p['type'] in ('smd', 'thru_hole', 'np_thru_hole') else 'smd', 'shape': p['shape'] if p['shape'] in ('rect', 'circle', 'oval', 'roundrect', 'trapezoid', 'custom', 'chamfered_rect') else 'rect',
                           'at': [p['x'], p['y']], 'size': [max(p['w'], 0.001), max(p['h'], 0.001)], 'angle': p['angle'], **({'drill': ([p['drill'], p['drill2']] if p['drill2'] != p['drill'] else p['drill'])} if p['type'] != 'smd' and p['drill'] > 0 else {})} for p in pads]})
(outdir / 'refs_all.json').write_text(json.dumps({'format': 'tracemaker-footprint-refs/1', 'references': refs}))
r = subprocess.run([tm, 'footprint-check', board, '--refs', str(outdir / 'refs_all.json'), '--trust-all', '--json', str(outdir / 'out_all.json')], capture_output=True, text=True)
j = json.loads((outdir / 'out_all.json').read_text())
st = collections.Counter(f['status'] for f in j['footprints'])
print(f"{pathlib.Path(board).name}: {len(chosen)} library footprints, exit {r.returncode}, statuses {dict(st)}")
for f in [f for f in j['footprints'] if f['status'] == 'mismatch'][:6]:
    print('  ', f['ref'], f['lib_id'], [(i['kind'], i['label']) for i in f['issues'][:3]])
