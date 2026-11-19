import json,glob,os,sys,collections
fail=[l.strip() for l in open('ROOT/bench/sets/pcbench-bd-fail.txt') if l.strip() and not l.startswith('#')]
best={}  # board -> (open, run, routed, conns, clean)
nruns=collections.Counter(); ever_clean=collections.defaultdict(list); ever_complete=collections.defaultdict(list)
for f in glob.glob('ROOT/../TraceMaker*/bench/results/*/boards.jsonl'):
    run=f.split('/')[-4].replace('TraceMaker','TM')+':'+f.split('/')[-2]
    try: s=json.load(open(os.path.dirname(f)+'/summary.json'))
    except Exception: s={}
    for l in open(f):
        try: r=json.loads(l)
        except Exception: continue
        b=r.get('board')
        if b not in fail or r.get('connections') in (None,0) or r.get('routed') is None: continue
        nruns[b]+=1
        ua=r.get('unconnected_after'); o=r['connections']-r['routed']
        added=sum((r.get('added_errors') or {}).values())
        key=(ua if ua is not None else 10**6, o)
        if r.get('clean'): ever_clean[b].append(run)
        if ua==0 and not added: ever_complete[b].append(run)
        if b not in best or key<best[b][0]:
            best[b]=(key,run,r['routed'],r['connections'],r.get('clean'),s.get('route_args'),s.get('work'),s.get('time_limit_s'),added)
for b in fail:
    if b in best:
        k,run,ro,c,cl,ra,w,t,ad=best[b]
        print(f"{b[:42]:42} runs={nruns[b]:3} best_unconn={k[0]:4} routed={ro}/{c} clean_runs={len(ever_clean[b]):3} complete_runs={len(ever_complete[b]):3} added={ad} | {run} | {ra} w={w} t={t}")
    else: print(b,'NO DATA')
