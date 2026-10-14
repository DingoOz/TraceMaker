"""For a board: a reference from the first front instance of every lib_id that is on both sides, then footprint-check on the whole board."""
import json, subprocess, sys, pathlib, collections
sys.path.insert(0, str(pathlib.Path(__file__).parent))
import sx
tm, board, outdir = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
outdir.mkdir(parents=True, exist_ok=True)
fps = sx.footprints(board)
by = collections.OrderedDict()
for f in fps:
    by.setdefault(f['lib'], []).append(f)
refs = []
chosen = []
for lib, l in by.items():
    front = [f for f in l if not f['back']]
    if not front or not any(f['back'] for f in l): continue
    src = front[0]
    pads = [p for p in src['pads'] if p['type'] == 'np_thru_hole' or any(x.endswith('.Cu') for x in p['layers'])]
    if len(pads) < 2: continue
    chosen.append(lib)
    refs.append({'brand': 'Scratch', 'part': lib, 'datasheet': {'document': 'derived from the front instance of ' + src['ref']}, 'match': {'lib_id': [lib.replace('[', '?').replace(']', '?')]},
                 'pads': [{'number': p['number'], 'type': p['type'] if p['type'] in ('smd', 'thru_hole', 'np_thru_hole') else 'smd', 'shape': p['shape'] if p['shape'] in ('rect', 'circle', 'oval', 'roundrect', 'trapezoid', 'custom', 'chamfered_rect') else 'rect',
                           'at': [p['x'], p['y']], 'size': [max(p['w'], 0.001), max(p['h'], 0.001)], 'angle': p['angle'], **({'drill': ([p['drill'], p['drill2']] if p['drill2'] != p['drill'] else p['drill'])} if p['type'] != 'smd' and p['drill'] > 0 else {})} for p in pads]})
(outdir / 'refs.json').write_text(json.dumps({'format': 'tracemaker-footprint-refs/1', 'references': refs}))
r = subprocess.run([tm, 'footprint-check', board, '--refs', str(outdir / 'refs.json'), '--trust-all', '--json', str(outdir / 'out.json')], capture_output=True, text=True)
print('exit', r.returncode, r.stderr[-300:])
j = json.loads((outdir / 'out.json').read_text())
st = collections.Counter()
bad = []
for f in j['footprints']:
    if f['lib_id'] in chosen:
        st[(f['status'], 'back' if next(x for x in fps if x['ref'] == f['ref'] and x['lib'] == f['lib_id'])['back'] else 'front')] += 1
        if f['status'] != 'ok': bad.append(f)
print(dict(st))
for f in bad[:8]:
    print(f['ref'], f['lib_id'], f['status'], f.get('alignment'), [ (i['kind'], i['message'][:90]) for i in f['issues'][:3]])
