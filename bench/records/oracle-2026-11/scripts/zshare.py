import json,re,os,collections
exec(open('compare.py').read().split("res={}")[0].split("# best run")[0])  # imports, FIX, fail
from importlib import util
segre=re.compile(r'\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer "?([\w.]+)"?\)\s+(?:\(locked\)\s+)?\(net "?([^")]*)"?\)')
segre2=re.compile(r'\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer "?([\w.]+)"?\)\s+\(net "?([^")]*)"?\)',re.S)
C=json.load(open('compare.json'))
def netmap(txt): return {a:n for a,n in re.findall(r'\(net (\d+) "?([^")]*)"?\)',txt)}
def pads_per_net(txt):
    c=collections.Counter()
    for a,n in re.findall(r'\(pad [^\n]*?\(net (\d+) "?([^")]*)"?\)',txt): c[n]+=1
    return c
tot_o=tot_z=0
print(f"{'board':32} zoneNets  zoneConns/all  our zone-net track mm / all  (share)   designer zone-net tracks mm")
for b,v in C.items():
    p=v['path'].replace('.route.json','')
    if not os.path.exists(p): print(b,'no pcb'); continue
    t=open(p,errors='replace').read(); nm=netmap(t)
    zn=set(v['zn'])
    S=[(float(w),nm.get(n,n),((float(x1)-float(x2))**2+(float(y1)-float(y2))**2)**.5) for x1,y1,x2,y2,w,L,n in segre.findall(t)]
    tot=sum(s[2] for s in S) or 1; z=sum(s[2] for s in S if s[1] in zn)
    un=open(FIX+b+'/unrouted.kicad_pcb',errors='replace').read()
    pp=pads_per_net(un); zc=sum(max(pp[n]-1,0) for n in zn); allc=sum(max(c-1,0) for n,c in pp.items() if n)
    print(f"{b[:32]:32} {len(zn):3}  {zc:5}/{allc:5} {zc/max(allc,1):5.0%}   {z:7.0f}/{tot:7.0f} {z/tot:5.0%}    {v['zonetracklen']:7.0f}  open={v['open']}  {','.join(sorted(zn))[:60]}")
