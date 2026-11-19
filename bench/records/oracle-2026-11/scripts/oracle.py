#!/usr/bin/env python3
"""Designer oracle: take the designer's routed board (processed.kicad_pcb), remove the copper of some nets, and
route only those nets with everything else fixed. Arms: open (the nets our best run left open), r25, r50 (random)."""
import json,re,os,sys,random,subprocess,pathlib,collections,concurrent.futures as cf,time
SP=pathlib.Path(__file__).resolve().parent
FIX='ROOT/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
OUT=pathlib.Path('ROOT/bench/results/oracle-2026-11-19')
TM=str(SP/'tm-frozen')
C=json.load(open(SP/'compare.json'))
start=re.compile(r'\n\s*\((segment|via|arc)\b')
def items(t):
    """(begin, end, net number) of every top-level track, via and arc"""
    out=[]; pos=0
    while True:
        m=start.search(t,pos)
        if not m: break
        i=t.index('(',m.start()); d=0; k=i
        while True:
            c=t[k]
            if c=='(': d+=1
            elif c==')':
                d-=1
                if d==0: break
            k+=1
        blk=t[i:k+1]; n=re.search(r'\(net (\d+)',blk)
        if n: out.append((m.start(),k+1,n[1]))
        pos=k+1
    return out
def prep(b,arm):
    t=open(FIX+b+'/processed.kicad_pcb',errors='replace').read()
    nm={}
    for m in re.finditer(r'^\s*\(net (\d+) (?:"([^"]*)"|([^\s)]+))\)\s*$',t,flags=re.M): nm[m[1]]=m[2] if m[2] is not None else m[3]
    name2num={v:k for k,v in nm.items()}
    its=items(t)
    have=collections.Counter(n for _,_,n in its)
    if not have: return None
    zn=set(C[b]['zn'])
    cand=sorted(n for n in have if nm.get(n) and nm[n] not in zn)
    if arm=='open':
        j=json.load(open(C[b]['path']))
        names=sorted({f.split(':')[0] for f in j.get('failures',[])})
        sel=[name2num[n] for n in names if n in name2num and name2num[n] in have and n not in zn]
    else:
        rnd=random.Random(1234+int(arm[1:]))
        sel=rnd.sample(cand,max(1,len(cand)*int(arm[1:])//100))
    if not sel: return None
    s=set(sel); parts=[]; pos=0
    for a,e,n in its:
        if n in s: parts.append(t[pos:a]); pos=e
    parts.append(t[pos:])
    d=OUT/b/arm; d.mkdir(parents=True,exist_ok=True)
    (d/'board.kicad_pcb').write_text(''.join(parts))
    json.dump([nm[n] for n in sel],open(d/'nets.json','w'))
    return d,[nm[n] for n in sel]
def run(job):
    i,b,arm=job
    p=prep(b,arm)
    if p is None: return dict(board=b,arm=arm,skip=True)
    d,nets=p
    pats=[re.escape(n) for n in nets]  # regular expressions, exact
    cmd=[TM,'route',str(d/'board.kicad_pcb'),'-o',str(d/'out.kicad_pcb'),'--json',str(d/'out.json'),'--no-kb','--no-gpu',
         '--work','20000000','--time','900','--variants','8','--threads','8','--seed','1','--view','--view-port',str(8900+i),
         '--only-net',','.join('^'+p+'$' for p in pats)]
    (d/'cmd.txt').write_text(' '.join(cmd))
    t0=time.time()
    r=subprocess.run(cmd,stdout=open(d/'log.txt','w'),stderr=subprocess.STDOUT)
    res=dict(board=b,arm=arm,nets=len(nets),exit=r.returncode,seconds=round(time.time()-t0))
    try:
        j=json.load(open(d/'out.json'))
        res.update(connections=j['connections'],routed=j['routed'],open=j['open'],why={k:v for k,v in j.get('why_open',{}).items() if isinstance(v,int)},fail=j.get('failures',[])[:40])
    except Exception as e: res['err']=str(e)
    with open(OUT/'results.jsonl','a') as f: f.write(json.dumps(res)+'\n')
    return res
boards=[b for b in C if C[b]['open']>0 or b in sys.argv[2:]]
arms=sys.argv[1].split(',')
jobs=[(i,b,a) for i,(a,b) in enumerate((a,b) for a in arms for b in boards)]
with cf.ThreadPoolExecutor(6) as ex:
    for r in ex.map(run,jobs): print({k:v for k,v in r.items() if k not in('fail',)},flush=True)
