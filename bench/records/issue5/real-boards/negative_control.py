"""Negative control: the references flipcheck.py derived, with pad 1 moved by 0.2 mm and pad 3 widened by 0.3 mm, must be called out on every footprint they apply to.
usage: negative_control.py <tracemaker> <board> <flipcheck-output-dir>"""
import json, subprocess, sys, pathlib
tm, board, d = sys.argv[1], sys.argv[2], pathlib.Path(sys.argv[3])
j = json.loads((d / 'refs.json').read_text())
for r in j['references']:
    r['pads'][0]['at'][0] += 0.2
    if len(r['pads']) > 2:
        r['pads'][2]['size'][0] += 0.3
(d / 'refs_bad.json').write_text(json.dumps(j))
p = subprocess.run([tm, 'footprint-check', board, '--refs', str(d / 'refs_bad.json'), '--trust-all', '--json', str(d / 'bad.json')], capture_output=True, text=True)
out = json.loads((d / 'bad.json').read_text())
applied = [f for f in out['footprints'] if f['status'] in ('ok', 'mismatch')]
print(f"exit {p.returncode}; applied to {len(applied)} footprints, {sum(1 for f in applied if f['status'] == 'mismatch')} called out; summary {out['summary']}")
kinds = {}
for f in applied:
    for i in f['issues']:
        kinds[i['kind']] = kinds.get(i['kind'], 0) + 1
print('issues by kind:', kinds)
