#!/usr/bin/env python3
"""Repair stage after a 20 M route from nothing, 33 failing PCBench boards: end windows (ends), along the bare-board route (route), both."""
import json,os,sys,subprocess,pathlib,time,concurrent.futures as cf
SP=pathlib.Path(__file__).resolve().parent
ROOT='ROOT/'
FIX=ROOT+'bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
OUT=pathlib.Path(ROOT+'bench/results/repair-2026-11-19'); OUT.mkdir(exist_ok=True)
TM=str(SP/'tm-repair')
boards=[l.strip() for l in open(ROOT+'bench/sets/pcbench-bd-fail.txt') if l.strip() and not l.startswith('#')]
ARMS={'ends':['--repair','on'],'route':['--repair','on','--repair-select','route'],'both':['--repair','on','--repair-select','both']}
arms=sys.argv[1:] or list(ARMS)
def run(job):
    i,b,arm=job
    d=OUT/b/arm; d.mkdir(parents=True,exist_ok=True)
    if (d/'out.json').exists(): return None
    cmd=[TM,'route',FIX+b+'/unrouted.kicad_pcb','-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu','--work','20000000','--time','1800',
         '--variants','8','--threads','8','--seed','1','--view','--view-port',str(9600+i)]+ARMS[arm]
    (d/'cmd.txt').write_text(' '.join(cmd))
    t0=time.time(); r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
    res=dict(board=b,arm=arm,exit=r.returncode,seconds=round(time.time()-t0))
    try:
        j=json.load(open(d/'out.json')); rp=j.get('repair') or {}
        res.update(connections=j['connections'],routed=j['routed'],open=j['open'],rep_before=rp.get('open_before'),rep_after=rp.get('open_after'),
                   attempts=rp.get('attempts'),kept=rp.get('kept'),rep_work=rp.get('work'),rep_s=rp.get('seconds'),no_bare=rp.get('no_bare_route'))
    except Exception as e: res['err']=str(e)
    with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
    return res
jobs=[(i,b,a) for i,(b,a) in enumerate((b,a) for b in boards for a in arms)]
with cf.ThreadPoolExecutor(6) as ex:
    for r in ex.map(run,jobs): print(r,flush=True)
