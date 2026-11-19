import json,glob,os,re,collections,sys
FIX='ROOT/bench/data/freerouting/scripts/benchmark/fixtures/PCBench/'
fail=[l.strip() for l in open('ROOT/bench/sets/pcbench-bd-fail.txt') if l.strip() and not l.startswith('#')]
# best run per board, native layers only (no c3-layers / closure)
best={}
for f in glob.glob('ROOT/../TraceMaker*/bench/results/*/boards.jsonl'):
    rd=os.path.dirname(f); run=os.path.basename(rd)
    if 'layers' in run or 'placed' in run or 'closure' in run or 'crules-off' in run or 'ghost' in run: continue
    for l in open(f):
        try: r=json.loads(l)
        except Exception: continue
        b=r.get('board')
        if b not in fail or not r.get('connections') or r.get('routed') is None: continue
        if sum((r.get('added_errors') or {}).values()): continue
        p=f"{rd}/boards/{b}.kicad_pcb.route.json"
        if not os.path.exists(p): continue
        o=r['connections']-r['routed']
        ua=r.get('unconnected_after'); key=(ua if ua is not None else 10**6,o)
        if b not in best or key<best[b][0]: best[b]=(key,p,r)
def nets_of(txt):
    m={}
    for a,n in re.findall(r'\(net (\d+) "?([^")]*)"?\)',txt): m[a]=n
    return m
segre=re.compile(r'\(segment\s+\(start ([-\d.]+) ([-\d.]+)\)\s+\(end ([-\d.]+) ([-\d.]+)\)\s+\(width ([\d.]+)\)\s+\(layer "?([\w.]+)"?\)\s+(?:\(locked\)\s+)?\(net "?([^")]*)"?\)')
def segs(txt):
    out=[]
    for x1,y1,x2,y2,w,L,n in segre.findall(txt):
        ln=((float(x1)-float(x2))**2+(float(y1)-float(y2))**2)**.5
        out.append((float(w),L,n,ln))
    return out
def zone_nets(txt,nm):
    zs=set()
    for a,n in re.findall(r'\(zone\s+\(net (\d+)\)\s+\(net_name "?([^")]*)"?\)',txt): zs.add(n)
    for n in re.findall(r'\(zone\s+\(net "([^"]*)"\)',txt): zs.add(n)
    return zs
def classes(txt):
    out=[]
    for m in re.finditer(r'\(net_class "?([^\s")]+)"?',txt):
        blk=txt[m.start():m.start()+600]
        g=lambda k:(re.search(r'\('+k+r' ([\d.]+)\)',blk) or [None,None])[1]
        out.append((m.group(1),g('clearance'),g('trace_width'),g('via_dia'),g('via_drill')))
    return out
res={}
print(f"{'board':30} {'open':>4} {'onZoneNet':>9} {'zoneNets':>8} | {'class clr/w/via/drill':28} | designer w: min/median(len-wt) share<classW | des vias | our vias | des len | our len | pitch | layers")
for b in fail:
    if b not in best: print(b,'no data'); continue
    key,p,r=best[b]
    j=json.load(open(p))
    raw=open(FIX+b+'/raw.kicad_pcb',errors='replace').read()
    un=open(FIX+b+'/unrouted.kicad_pcb',errors='replace').read()
    nm=nets_of(raw)
    zn=zone_nets(raw,nm); zn.discard('')
    S=segs(raw); S=[(w,L,(nm.get(n,n)),ln) for w,L,n,ln in S]
    tot=sum(s[3] for s in S) or 1
    ws=sorted((s[0],s[3]) for s in S)
    acc=0;med=None
    for w,ln in ws:
        acc+=ln
        if acc>=tot/2: med=w;break
    cl=classes(un) or classes(raw)
    cw=float(cl[0][2]) if cl and cl[0][2] else None
    below=sum(ln for w,ln in ws if cw and w<cw-1e-6)/tot if cw else float('nan')
    fails=j.get('failures',[])
    fn=[f.split(':')[0] for f in fails]
    onz=sum(1 for n in fn if n in zn)
    desvias=len(re.findall(r'\(via\s',raw))
    zlen=sum(s[3] for s in S if s[2] in zn)
    res[b]=dict(open=len(fails),onz=onz,zn=sorted(zn),cls=cl[:3],minw=ws[0][0] if ws else None,med=med,below=below,desvias=desvias,ourvias=j.get('vias'),deslen=tot,ourlen=j.get('style',{}).get('length_mm'),pitch=j.get('pitch_mm'),layers=j.get('copper_layers'),path=p,failnets=collections.Counter(fn).most_common(6),zonetracklen=zlen)
    c=cl[0] if cl else ('?',)*5
    print(f"{b[:30]:30} {len(fails):4} {onz:9} {len(zn):8} | {str(c[1:]):28} | {ws[0][0] if ws else 0:.3f}/{med} {below:5.0%} | {desvias:5} | {j.get('vias'):5} | {tot:7.0f} | {j.get('style',{}).get('length_mm',0):7.0f} | {j.get('pitch_mm')} | {j.get('copper_layers')}")
json.dump(res,open('compare.json','w'),indent=1)
