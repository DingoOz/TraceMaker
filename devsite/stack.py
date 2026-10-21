#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Live page for the stack tests: whole PCBench tiers A-D and the srj19/srj20 sets, run to 100 % with escalating time.

Reads bench/results/{full0100-tier*,stack-*}/ (boards.jsonl, summary.json), the ledger of bench/ledger.py, running router processes and the load.
Read-only; standard library only. Usage: devsite/stack.py [--host 0.0.0.0] [--port 8771]
"""
from __future__ import annotations

import argparse
import json
import os
import pathlib
import re
import subprocess
import sys
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

ROOT = pathlib.Path(__file__).resolve().parent.parent
RES = ROOT / "bench" / "results"
sys.path.insert(0, str(ROOT / "bench"))
import ledger  # noqa: E402

TOTAL = {"srj19": 200, "srj20": 200, "tierD": 22, "tierC": 122, "tierB": 560, "tierA": 453}
ORDER = ["srj19", "srj20", "tierD", "tierC", "tierB", "tierA"]
_cache: dict = {}
_lock = threading.Lock()


def cached(key, ttl, fn):
    now = time.time()
    with _lock:
        hit = _cache.get(key)
        if hit and now - hit[0] < ttl:
            return hit[1]
    v = fn()
    with _lock:
        _cache[key] = (now, v)
    return v


def processes():
    out = []
    try:
        txt = subprocess.run(["ps", "-eo", "pid,etimes,args"], capture_output=True, text=True, timeout=5).stdout
    except (OSError, subprocess.SubprocessError):
        return out
    for line in txt.splitlines()[1:]:
        m = re.match(r"\s*(\d+)\s+(\d+)\s+(\S*/bench/results/(\S+?)/tracemaker) route (\S+)", line)
        if m:
            p = pathlib.Path(m.group(5))
            port = re.search(r" --view-port (\d+)", line)
            tok = re.search(r" --view-token (\S+)", line)
            out.append({"pid": int(m.group(1)), "elapsed": int(m.group(2)), "run": m.group(4), "board": p.parent.name if p.name == "unrouted.kicad_pcb" else p.stem,
                        "view_port": int(port.group(1)) if port else None, "view_token": tok.group(1) if tok else None})
    return out


def state():
    led = ledger.build()
    sets = {}
    for k in ORDER:
        es = [e for e in led.values() if e["set"] == k]
        cnt = {s: sum(1 for e in es if e["state"] == s) for s in ("clean", "open", "defect", "capped", "fixture")}
        sets[k] = {"total": TOTAL[k], "tested": len(es), **cnt}
    runs = []
    for key, stage, d in ledger.runs():
        f = d / "boards.jsonl"
        rows = [json.loads(l) for l in f.read_text().splitlines()] if f.exists() else []
        summ = d / "summary.json"
        listed = None
        log = RES / f"{d.name}.log"
        try:
            listed = int(re.search(r": (\d+) boards", log.read_text().split("\n", 1)[0]).group(1))
        except (OSError, AttributeError):
            pass
        s = json.loads(summ.read_text()) if summ.exists() else {}
        runs.append({"run": d.name, "set": key, "stage": stage, "boards": listed or s.get("boards") or len(rows), "done": len(rows), "clean": sum(1 for r in rows if r.get("clean")),
                     "finished": summ.exists(), "time_s": s.get("time_limit_s"), "work": s.get("work"), "args": s.get("route_args", ""), "started": d.stat().st_ctime})
    openb = []
    for e in sorted(led.values(), key=lambda e: (ORDER.index(e["set"]), e["board"])):
        if e["clean"]:
            continue
        b = e["best"]
        openb.append({"set": e["set"], "board": e["board"], "state": e["state"], "attempts": len(e["attempts"]), "run": b["run"], "open": b["open"], "of": b.get("connections"),
                      "added": b["added"], "gate": b["gate"], "spent": e["total_seconds"]})
    return {"now": time.time(), "load": os.getloadavg(), "sets": sets, "runs": runs, "open": openb, "procs": processes(),
            "burndown": cached("burn", 60, lambda: ledger.burndown(TOTAL))}


PAGE = r"""<!doctype html><meta charset=utf-8><title>TraceMaker stack tests</title>
<style>
body{background:#0b1220;color:#dbe4f3;font:14px/1.45 system-ui,sans-serif;margin:0;padding:20px 28px}
h1{font-size:22px;margin:0 0 4px}h2{font-size:15px;margin:26px 0 8px;color:#8fb4ff;text-transform:uppercase;letter-spacing:.08em}
.sub{color:#7d8ba3;margin-bottom:14px}.grid{display:grid;grid-template-columns:repeat(auto-fill,minmax(310px,1fr));gap:12px}
.card{background:#121c30;border:1px solid #22314f;border-radius:10px;padding:12px 14px}.card b{font-size:17px}
.bar{height:9px;background:#1d2a45;border-radius:5px;overflow:hidden;margin:8px 0;display:flex}.bar i{display:block;height:100%}
.g{background:#33d17a}.a{background:#f5a524}.r{background:#ff5d5d}.p{background:#8a94a8}.big{font-size:30px;font-weight:700}
table{border-collapse:collapse;width:100%;font-size:13px}th,td{text-align:left;padding:3px 9px;border-bottom:1px solid #1b2742}th{color:#7d8ba3;font-weight:500}
td.n,th.n{text-align:right;font-variant-numeric:tabular-nums}.tag{padding:1px 7px;border-radius:9px;font-size:11px}
.t-open{background:#4a3410;color:#f5a524}.t-defect{background:#4a1a1a;color:#ff7b7b}.t-capped,.t-fixture{background:#2a2f3d;color:#aab3c6}.t-run{background:#10391f;color:#58e69a}
.live{display:grid;grid-template-columns:repeat(auto-fill,minmax(560px,1fr));gap:14px}.lv{background:#121c30;border:1px solid #22314f;border-radius:10px;overflow:hidden}
.lv header{padding:6px 12px;display:flex;justify-content:space-between;font-size:13px}.lv iframe{width:100%;height:440px;border:0;background:#000;display:block}
.lv a{color:#8fb4ff}.muted{color:#7d8ba3}.scroll{max-height:420px;overflow:auto}
</style>
<h1>TraceMaker stack tests</h1>
<div class=sub>Whole PCBench tiers A-D and the srj19/srj20 BGA sets, current main. Goal: 100 % clean, speed second; open boards are continued with more time (bench/escalate.py).
 <span id=meta></span></div>
<div class=grid id=sets></div>
<h2>Burndown</h2><div class=card><div class=sub style="margin:0 0 6px">Boards not yet clean (D106 judge) over the campaign; untested boards count as not clean. Each step is a board turning clean; the time is when its run wrote the result.</div>
<div id=burn></div></div>
<h2>Running now</h2><div id=procs class=card></div>
<h2>Live routing</h2><div class=sub>The router's own viewer for each board being routed, streamed straight from the engine (only the first portfolio variant is drawn). Boards that started before the live view existed have no panel.</div>
<div id=live class=live></div><div id=livenote class=muted></div>
<h2>Runs</h2><div class=card><table id=runs></table></div>
<h2>Boards not yet clean</h2><div class="card scroll"><table id=open></table></div>
<script>
const f=(n,d=0)=>n==null?'':Number(n).toLocaleString(undefined,{maximumFractionDigits:d});
const dur=s=>s>=3600?(s/3600).toFixed(1)+' h':s>=60?Math.round(s/60)+' min':s+' s';
const COL={total:'#e8eefc',srj19:'#33d17a',srj20:'#58c4ff',tierD:'#ff7b7b',tierC:'#f5a524',tierB:'#c792ff',tierA:'#ffd866'};
function burn(b){
 const box=document.getElementById('burn');
 if(!b||!b.total.length){box.textContent='no results yet';return}
 const W=1180,H=340,L=52,R=16,T=12,B=30,t0=b.t0,t1=b.total[b.total.length-1][0],ymax=b.total[0][1];
 const X=t=>L+(W-L-R)*(t-t0)/Math.max(1,t1-t0),Y=v=>T+(H-T-B)*(1-v/ymax);
 const step=pts=>{let d='M'+X(pts[0][0]).toFixed(1)+' '+Y(pts[0][1]).toFixed(1);for(let i=1;i<pts.length;i++)d+='H'+X(pts[i][0]).toFixed(1)+'V'+Y(pts[i][1]).toFixed(1);return d};
 let g='';
 const ys=ymax>800?250:ymax>300?100:ymax>100?50:10;for(let v=0;v<=ymax;v+=ys){g+=`<line x1=${L} x2=${W-R} y1=${Y(v)} y2=${Y(v)} stroke="#1b2742"/><text x=${L-6} y=${Y(v)+4} text-anchor=end fill="#7d8ba3" font-size=11>${Math.round(v)}</text>`}
 const hrs=(t1-t0)/3600,hs=hrs>30?6:hrs>12?3:hrs>5?2:1;
 for(let h=0;h<=hrs;h+=hs){const t=t0+h*3600;g+=`<line x1=${X(t)} x2=${X(t)} y1=${T} y2=${H-B} stroke="#16213a"/><text x=${X(t)} y=${H-B+16} text-anchor=middle fill="#7d8ba3" font-size=11>${new Date(t*1000).toLocaleTimeString([],{hour:'2-digit',minute:'2-digit'})}</text>`}
 for(const k of Object.keys(b.series))g+=`<path d="${step(b.series[k])}" fill=none stroke="${COL[k]||'#888'}" stroke-width=1.6 opacity=.9 />`;
 g+=`<path d="${step(b.total)}" fill=none stroke="${COL.total}" stroke-width=2.8 />`;
 const cur=k=>{const p=(k==='total'?b.total:b.series[k]);return p[p.length-1][1]};
 let lg='<div style="display:flex;gap:16px;flex-wrap:wrap;margin-top:6px">';
 for(const k of ['total',...Object.keys(b.series)])lg+=`<span><i style="display:inline-block;width:14px;height:3px;background:${COL[k]||'#888'};vertical-align:middle;margin-right:5px"></i>${k} <b>${cur(k)}</b></span>`;
 box.innerHTML=`<svg viewBox="0 0 ${W} ${H}" style="width:100%;height:auto">${g}</svg>`+lg+'</div>';
}
// One iframe per viewer port. The set is diffed, never rebuilt, so the streams keep running between updates.
const frames=new Map();
function live(procs){
 const box=document.getElementById('live'),want=new Map();
 for(const x of procs)if(x.view_port)want.set(x.view_port,x);
 for(const [port,el] of frames)if(!want.has(port)||el.dataset.board!==want.get(port).board){el.remove();frames.delete(port)}
 for(const [port,x] of want){
  const url=`http://${location.hostname}:${port}/?token=${x.view_token}`;
  let el=frames.get(port);
  if(!el){el=document.createElement('div');el.className='lv';el.dataset.board=x.board;
   el.innerHTML=`<header><b>${x.board}</b><span class=muted>${x.run} · <span class=el></span> · <a target=_blank href="${url}">open</a></span></header><iframe src="${url}" allow="fullscreen"></iframe>`;
   box.appendChild(el);frames.set(port,el)}
  el.querySelector('.el').textContent=dur(x.elapsed);
 }
 const none=procs.filter(x=>!x.view_port).length;
 document.getElementById('livenote').textContent=want.size?'':'No board is streaming right now.'+(none?` (${none} running without a live view)`:'');
}
async function tick(){
 let s;try{s=await (await fetch('/api/state')).json()}catch(e){return}
 document.getElementById('meta').textContent=' load '+s.load.map(x=>x.toFixed(0)).join(' / ')+' · '+new Date(s.now*1000).toLocaleTimeString();
 let h='';
 for(const k of Object.keys(s.sets)){const v=s.sets[k],t=v.total,w=x=>(100*x/t).toFixed(2)+'%';
  const un=t-v.tested;
  h+=`<div class=card><b>${k}</b> <span class=muted>${v.tested}/${t} tested</span><div class=big>${f(100*v.clean/t,1)}%<span class=muted style="font-size:14px"> ${v.clean}/${t} clean</span></div>
  <div class=bar><i class=g style="width:${w(v.clean)}"></i><i class=a style="width:${w(v.open)}"></i><i class=r style="width:${w(v.defect)}"></i><i class=p style="width:${w(v.capped+(v.fixture||0))}"></i></div>
  <span class=muted>open ${v.open} · added error ${v.defect} · report-capped ${v.capped}${v.fixture?' · bad fixture '+v.fixture:''} · untested ${un}</span></div>`}
 document.getElementById('sets').innerHTML=h;
 let p=s.procs.sort((a,b)=>b.elapsed-a.elapsed).map(x=>`<tr><td>${x.board}</td><td>${x.run}</td><td class=n>${dur(x.elapsed)}</td></tr>`).join('');
 document.getElementById('procs').innerHTML=s.procs.length?`<table><tr><th>board<th>run<th class=n>running</tr>${p}</table>`:'<span class=muted>nothing routing right now</span>';
 live(s.procs);burn(s.burndown);
 document.getElementById('runs').innerHTML='<tr><th>run<th>stage<th class=n>boards<th class=n>done<th class=n>clean<th>limit<th>status<th>route options</tr>'+
  s.runs.sort((a,b)=>b.started-a.started).map(r=>`<tr><td>${r.run}<td>${r.stage}<td class=n>${r.boards}<td class=n>${r.done}<td class=n>${r.clean}<td>${r.work?f(r.work/1e6)+' M work':r.time_s?dur(r.time_s):''}<td>${r.finished?'finished':'<span class="tag t-run">running</span>'}<td class=muted>${r.args}</tr>`).join('');
 document.getElementById('open').innerHTML='<tr><th>set<th>board<th>state<th class=n>attempts<th>best run<th class=n>open<th>added errors<th class=n>time spent</tr>'+
  s.open.map(b=>`<tr><td>${b.set}<td>${b.board}<td><span class="tag t-${b.state}">${b.state}</span><td class=n>${b.attempts}<td>${b.run}<td class=n>${b.open}${b.of?' / '+b.of:''}<td>${Object.entries(b.added||{}).map(([k,v])=>k+' '+v).join(', ')}<td class=n>${dur(Math.round(b.spent))}</tr>`).join('');
}
tick();setInterval(tick,10000);
</script>"""


class H(BaseHTTPRequestHandler):
    def log_message(self, *a):
        pass

    def do_GET(self):
        if self.path.startswith("/api/state"):
            body, ct = json.dumps(cached("state", 8, state)).encode(), "application/json"
        elif self.path == "/ledger.md":
            body, ct = (RES / "stack-ledger.md").read_bytes() if (RES / "stack-ledger.md").exists() else b"", "text/plain; charset=utf-8"
        else:
            body, ct = PAGE.encode(), "text/html; charset=utf-8"
        self.send_response(200)
        self.send_header("Content-Type", ct)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="0.0.0.0")
    ap.add_argument("--port", type=int, default=8771)
    a = ap.parse_args()
    ThreadingHTTPServer((a.host, a.port), H).serve_forever()
