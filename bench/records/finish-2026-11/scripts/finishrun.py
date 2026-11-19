#!/usr/bin/env python3
"""route --finish end to end (the product command): 20 M, --finish-masks --finish-layers 2,4, on the boards open at base, the six largest left out.
Three jobs at a time; a job waits while less than 40 GB of memory is available."""
import json,os,sys,subprocess,pathlib,time,concurrent.futures as cf
SP=pathlib.Path(__file__).resolve().parent
ROOT='ROOT/'
FIX=ROOT+'bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
OUT=pathlib.Path(ROOT+'bench/results/finish-2026-11-19'); OUT.mkdir(exist_ok=True)
TM=str(SP/'tm-fin')
SKIP={'Aleste-520EX_aleste','kitspace_d20_tri_r0.3','MonApollo_analog-board','mechkeys_LFK78','zx-sizif-512-ext_sizif512ext','poncho_fpga_fpga-dongle',
      'Hardware_Playground_multisensor_cr2032','front-end-modules_LimeSDR_Sony','mechkeys_58r-mxalps'}
base={}
for l in open(ROOT+'bench/results/fit-2026-11-19/results.jsonl'):
    r=json.loads(l)
    if r['arm']=='base': base[r['board']]=(r['seconds'],r['open'])
boards=sorted((b for b in base if b not in SKIP),key=lambda b:base[b][0])
def avail():
    for l in open('/proc/meminfo'):
        if l.startswith('MemAvailable'): return int(l.split()[1])/1e6
def run(job):
    i,b=job
    d=OUT/b; d.mkdir(parents=True,exist_ok=True)
    if (d/'out.json').exists(): return None
    while avail()<40: time.sleep(30)
    cmd=[TM,'route',FIX+b+'/unrouted.kicad_pcb','-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu','--work','20000000','--time','3600',
         '--variants','8','--threads','8','--seed','1','--view','--view-port',str(10500+i),'--finish','--finish-masks','--finish-layers','2,4']
    (d/'cmd.txt').write_text(' '.join(cmd))
    t0=time.time(); r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
    res=dict(board=b,exit=r.returncode,seconds=round(time.time()-t0),base_open=base[b][1])
    try:
        j=json.load(open(d/'out.json')); f=j['finish']
        res.update(open=f['open'],complete=f['complete'],layers=j.get('copper_layers'),rungs=[(x['rung'],x['open_after'],x['kept']) for x in f['rungs']])
    except Exception as e: res['err']=str(e)
    with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
    return res
with cf.ThreadPoolExecutor(3) as ex:
    for r in ex.map(run,list(enumerate(boards))): print(r,flush=True)
