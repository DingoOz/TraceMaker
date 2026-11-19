#!/usr/bin/env python3
"""Pad-mask rung on top: topmask = on topfit's output with --soft-pad-masks (explicit); topmaskref = on topfitref's output, fit from the designer's
layout with the new binary (pad masks soft only where the designer's tracks cross them)."""
import json,os,sys,subprocess,pathlib,time,concurrent.futures as cf
SP=pathlib.Path(__file__).resolve().parent
ROOT='ROOT/'
FIX=ROOT+'bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
OUT=pathlib.Path(ROOT+'bench/results/ontop-2026-11-19')
TM=str(SP/'tm-pm')
boards=[l.strip() for l in open(ROOT+'bench/sets/pcbench-bd-fail.txt') if l.strip() and not l.startswith('#')]
EVID={'memsarray_mems_modules','poncho_fpga_fpga-dongle','zx-sizif-512-ext_sizif512ext','dorkyboard_keyboard','mechkeys_LFK78'}
REP=['--repair','on','--repair-select','both']
def run(job):
    i,b,arm=job
    prev='topfit' if arm=='topmask' else 'topfitref'
    if json.load(open(OUT/b/prev/'out.json'))['open']==0: return None
    if arm=='topmaskref' and b not in EVID: return None
    d=OUT/b/arm; d.mkdir(parents=True,exist_ok=True)
    if (d/'out.json').exists(): return None
    extra=(['--fit-rules-from',FIX+b+'/unrouted.kicad_pcb','--soft-pad-masks'] if arm=='topmask' else ['--fit-rules-from',FIX+b+'/processed.kicad_pcb'])+REP
    cmd=[TM,'route',str(OUT/b/prev/'out.kicad_pcb'),'-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu','--work','20000000','--time','3600',
         '--variants','8','--threads','8','--seed','1','--view','--view-port',str(10100+i)]+extra
    (d/'cmd.txt').write_text(' '.join(cmd))
    t0=time.time(); r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
    res=dict(board=b,arm=arm,exit=r.returncode,seconds=round(time.time()-t0))
    try:
        j=json.load(open(d/'out.json')); res.update(connections=j['connections'],routed=j['routed'],open=j['open'])
    except Exception as e: res['err']=str(e)
    with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
    return res
jobs=[(i,b,a) for i,(b,a) in enumerate((b,a) for a in ('topmaskref','topmask') for b in boards)]
with cf.ThreadPoolExecutor(4) as ex:
    for r in ex.map(run,jobs):
        if r: print(r,flush=True)
