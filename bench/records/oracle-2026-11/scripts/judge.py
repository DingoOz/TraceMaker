import sys,json,pathlib,collections
sys.path.insert(0,'ROOT/bench')
import run as bench
out={}
for b in sys.argv[1:]:
    row={}
    for arm in ('fit3','fitref3'):
        p=pathlib.Path(f'ROOT/bench/results/fit-2026-11-19/{b}/{arm}/out.kicad_pcb')
        j=bench.drc(p); u=bench.drc(bench.FIX/b/'unrouted.kicad_pcb')
        if j is None or u is None: row[arm]='drc failed'; continue
        added={k:v-u['routed_errors'].get(k,0) for k,v in j['routed_errors'].items() if v-u['routed_errors'].get(k,0)>0}
        row[arm]=dict(unconnected=j['unconnected'],added=added)
    out[b]=row; print(b,json.dumps(row),flush=True)
json.dump(out,open('ROOT/bench/results/fit-2026-11-19/kicad-judge.json','w'),indent=1)
