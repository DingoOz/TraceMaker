#!/usr/bin/env python3
"""Rungs on top of the default-rules result (base, 20 M): only its open connections are routed, its copper stays unless a repair window lifts it.
topfit: --fit-rules + repair; topfitref: fit from the designer's layout + repair; ctl60: default rules from nothing at 60 M (3x the base budget)."""
import json,os,sys,subprocess,pathlib,time,concurrent.futures as cf
SP=pathlib.Path(__file__).resolve().parent
ROOT='ROOT/'
FIX=ROOT+'bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
BASE=ROOT+'bench/results/fit-2026-11-19/'
OUT=pathlib.Path(ROOT+'bench/results/ontop-2026-11-19'); OUT.mkdir(exist_ok=True)
TM=str(SP/'tm-repair')
boards=[l.strip() for l in open(ROOT+'bench/sets/pcbench-bd-fail.txt') if l.strip() and not l.startswith('#')]
REP=['--repair','on','--repair-select','both']
def args(b,arm):
    top=BASE+b+'/base/out.kicad_pcb'
    if arm=='topfit': return top,['--fit-rules']+REP,'20000000'
    if arm=='topfitref': return top,['--fit-rules-from',FIX+b+'/processed.kicad_pcb']+REP,'20000000'
    if arm=='ctl60': return FIX+b+'/unrouted.kicad_pcb',[],'60000000'
arms=sys.argv[1:]
def run(job):
    i,b,arm=job
    d=OUT/b/arm; d.mkdir(parents=True,exist_ok=True)
    if (d/'out.json').exists(): return None
    inp,extra,work=args(b,arm)
    cmd=[TM,'route',inp,'-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu','--work',work,'--time','3600',
         '--variants','8','--threads','8','--seed','1','--view','--view-port',str(9800+i)]+extra
    (d/'cmd.txt').write_text(' '.join(cmd))
    t0=time.time(); r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
    res=dict(board=b,arm=arm,exit=r.returncode,seconds=round(time.time()-t0))
    try:
        j=json.load(open(d/'out.json')); rp=j.get('repair') or {}
        res.update(connections=j['connections'],routed=j['routed'],open=j['open'],rep_before=rp.get('open_before'),rep_after=rp.get('open_after'),rep_work=rp.get('work'),
                   work_all=j.get('work_all_variants'),time_limited=j.get('time_limited'))
    except Exception as e: res['err']=str(e)
    with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
    return res
jobs=[(i,b,a) for i,(b,a) in enumerate((b,a) for b in boards for a in arms)]
with cf.ThreadPoolExecutor(6) as ex:
    for r in ex.map(run,jobs): print(r,flush=True)
