#!/usr/bin/env python3
"""Seed spread: the boards within 3 connections of complete after topfit, routed from nothing with --fit-rules at seeds 2-5 (seed 1 = fit3)."""
import json,os,sys,subprocess,pathlib,time,concurrent.futures as cf
SP=pathlib.Path(__file__).resolve().parent
ROOT='ROOT/'
FIX=ROOT+'bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
OUT=pathlib.Path(ROOT+'bench/results/ontop-2026-11-19')
TM=str(SP/'tm-pm')
boards=['96boards-robomezzi_96boards-robomezzi','Hardware_Playground_serial_gw_ATMEGA328P','KiCad-Library_Teensy_test_layout','LeeChee_1800','dorkyboard_keyboard',
        'PCIE-to-MXM-Adapter_PCIEx1toMXM3.0','kitspace_teensy-fx','motor-high-current-sensors_CurrentSensorPCB_line']
def run(job):
    i,b,seed=job; arm=f'fitseed{seed}'
    d=OUT/b/arm; d.mkdir(parents=True,exist_ok=True)
    if (d/'out.json').exists(): return None
    cmd=[TM,'route',FIX+b+'/unrouted.kicad_pcb','-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu','--work','20000000','--time','3600',
         '--variants','8','--threads','8','--seed',str(seed),'--view','--view-port',str(10200+i),'--fit-rules']
    (d/'cmd.txt').write_text(' '.join(cmd))
    t0=time.time(); r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
    res=dict(board=b,arm=arm,exit=r.returncode,seconds=round(time.time()-t0))
    try:
        j=json.load(open(d/'out.json')); res.update(connections=j['connections'],routed=j['routed'],open=j['open'])
    except Exception as e: res['err']=str(e)
    with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
    return res
jobs=[(i,b,s) for i,(b,s) in enumerate((b,s) for b in boards for s in (2,3,4,5))]
with cf.ThreadPoolExecutor(5) as ex:
    for r in ex.map(run,jobs): print(r,flush=True)
