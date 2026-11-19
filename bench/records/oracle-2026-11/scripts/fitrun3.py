#!/usr/bin/env python3
"""Rule fit from nothing: the 33 failing PCBench boards, 20 M work, 8 variants; arms base, fit (own pads), fitref (designer's layout)."""
import json,os,sys,subprocess,pathlib,time,concurrent.futures as cf
SP=pathlib.Path(__file__).resolve().parent
ROOT='ROOT/'
FIX=ROOT+'bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
OUT=pathlib.Path(ROOT+'bench/results/fit-2026-11-19'); OUT.mkdir(exist_ok=True)
TM=str(SP/'tm-fit3')
boards=[l.strip() for l in open(ROOT+'bench/sets/pcbench-bd-fail.txt') if l.strip() and not l.startswith('#')]
ARMS={'fit3':['--fit-rules'],'fitref3':None}
def run(job):
    i,b,arm=job
    d=OUT/b/arm; d.mkdir(parents=True,exist_ok=True)
    if (d/'out.json').exists(): return None
    extra=ARMS[arm] if ARMS[arm] is not None else ['--fit-rules-from',FIX+b+'/processed.kicad_pcb']
    cmd=[TM,'route',FIX+b+'/unrouted.kicad_pcb','-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu','--work','20000000','--time','1800',
         '--variants','8','--threads','8','--seed','1','--view','--view-port',str(9500+i)]+extra
    (d/'cmd.txt').write_text(' '.join(cmd))
    t0=time.time(); r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
    res=dict(board=b,arm=arm,exit=r.returncode,seconds=round(time.time()-t0))
    try:
        j=json.load(open(d/'out.json')); res.update(connections=j['connections'],routed=j['routed'],open=j['open'],fitted=len(j.get('fitted_rules',{}).get('changes',[])))
    except Exception as e: res['err']=str(e)
    with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
    return res
jobs=[(i,b,a) for i,(b,a) in enumerate((b,a) for b in boards for a in ARMS)]
with cf.ThreadPoolExecutor(6) as ex:
    for r in ex.map(run,jobs): print(r,flush=True)
