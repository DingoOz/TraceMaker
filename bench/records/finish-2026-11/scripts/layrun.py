#!/usr/bin/env python3
"""Layer rungs on top of topfit: boards still open get +2 copper layers (toplay2), then +4 (toplay4), each on the output of the rung before."""
import json,os,sys,subprocess,pathlib,time,concurrent.futures as cf
SP=pathlib.Path(__file__).resolve().parent
ROOT='ROOT/'
OUT=pathlib.Path(ROOT+'bench/results/ontop-2026-11-19')
TM=str(SP/'tm-repair')
boards=[l.strip() for l in open(ROOT+'bench/sets/pcbench-bd-fail.txt') if l.strip() and not l.startswith('#')]
def run(job):
    i,b=job; out=[]
    prev='topfit'
    for arm,add in (('toplay2',2),('toplay4',4)):
        pj=json.load(open(OUT/b/prev/'out.json'))
        if pj['open']==0: break
        L0=json.load(open(OUT/b/'topfit'/'out.json'))['copper_layers']
        d=OUT/b/arm; d.mkdir(parents=True,exist_ok=True)
        if not (d/'out.json').exists():
            cmd=[TM,'route',str(OUT/b/prev/'out.kicad_pcb'),'-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu','--work','20000000','--time','3600',
                 '--variants','8','--threads','8','--seed','1','--view','--view-port',str(9900+i),'--fit-rules','--copper-layers',str(L0+add),'--repair','on','--repair-select','both']
            (d/'cmd.txt').write_text(' '.join(cmd))
            t0=time.time(); r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
            res=dict(board=b,arm=arm,exit=r.returncode,seconds=round(time.time()-t0))
            try:
                j=json.load(open(d/'out.json')); res.update(connections=j['connections'],routed=j['routed'],open=j['open'],layers=j['copper_layers'])
            except Exception as e: res['err']=str(e); 
            with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
            out.append(res)
            if 'err' in res: break
        prev=arm
    return out
with cf.ThreadPoolExecutor(6) as ex:
    for r in ex.map(run,list(enumerate(boards))): print(r,flush=True)
