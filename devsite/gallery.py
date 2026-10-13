#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-3.0-or-later
"""Gallery of routed tscircuit boards (srj19, srj20): renders the best result of each board found in bench/results/ to build/gallery/*.png
and serves a page with thumbnails, connection counts and vias. Standard library plus bench/render_srj.py (Pillow).

Usage: devsite/gallery.py [--per-set 30] [--port 8768] [--no-render]
"""
import argparse, glob, json, pathlib, subprocess, sys, http.server, functools

ROOT = pathlib.Path(__file__).resolve().parent.parent
OUT = ROOT / "build/gallery"


def best_results(setname):
    """board -> (clean, open, run, row), best over every run of that set with boards on disk."""
    best = {}
    for jl in glob.glob(str(ROOT / "bench/results/*/boards.jsonl")):
        run = pathlib.Path(jl).parent
        try:
            s = json.load(open(run / "summary.json"))
        except Exception:
            continue
        if s.get("set") != setname:
            continue
        for line in open(jl):
            r = json.loads(line)
            pcb = run / "boards" / (r["board"] + ".kicad_pcb")
            if not pcb.exists():
                continue
            key = (0 if r["clean"] else 1, r["connections"] - r["routed"])
            if r["board"] not in best or key < best[r["board"]][0]:
                best[r["board"]] = (key, run, r)
    return best


def render(per_set):
    items = []
    for name in ("srj19", "srj20"):
        best = best_results(name)
        picks = sorted(best, key=lambda b: (best[b][0][0], -best[b][2]["connections"]))  # clean first, biggest first
        clean = [b for b in picks if best[b][0][0] == 0][::max(1, len([b for b in picks if best[b][0][0] == 0]) // per_set)][:per_set]
        for b in clean:
            _, run, r = best[b]
            png = OUT / f"{name}_{b}.png"
            if not png.exists():
                prob = ROOT / f"bench/data/tscircuit/boards/{name}/{b}/problem.json"
                subprocess.run([sys.executable, str(ROOT / "bench/render_srj.py"), str(prob), str(run / "boards" / (b + ".kicad_pcb")), str(png)],
                               check=False, capture_output=True)
            if png.exists():
                items.append({"set": name, "board": b, "png": png.name, "connections": r["connections"], "vias": r["vias"], "run": run.name})
    json.dump(items, open(OUT / "index.json", "w"), indent=1)
    return items


PAGE = """<!doctype html><meta charset=utf-8><title>TraceMaker: routed boards</title>
<style>body{font:14px system-ui;background:#111;color:#ddd;margin:20px}h1{font-weight:500}
.g{display:grid;grid-template-columns:repeat(auto-fill,minmax(260px,1fr));gap:12px}
.c{background:#1b1b1b;border:1px solid #333;border-radius:6px;padding:6px}.c img{width:100%%;background:#fff;border-radius:3px}
.c a{color:#8bf}.m{color:#999;font-size:12px}</style>
<h1>TraceMaker: %d routed boards (fully connected, KiCad DRC clean)</h1>
<p class=m>tscircuit BGA breakouts srj19 and srj20; front layer blue, back layer red, vias green. Best run of each board in bench/results.</p>
<div class=g>%s</div>"""


def page(items):
    cards = "".join(f'<div class=c><a href="{i["png"]}"><img loading=lazy src="{i["png"]}"></a><div>{i["set"]} {i["board"]}</div>'
                    f'<div class=m>{i["connections"]} connections, {i["vias"]} vias, run {i["run"]}</div></div>' for i in items)
    return (PAGE % (len(items), cards)).encode()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--per-set", type=int, default=30)
    ap.add_argument("--port", type=int, default=8768)
    ap.add_argument("--no-render", action="store_true")
    a = ap.parse_args()
    OUT.mkdir(parents=True, exist_ok=True)
    items = json.load(open(OUT / "index.json")) if a.no_render else render(a.per_set)
    html = page(items)

    class H(http.server.SimpleHTTPRequestHandler):
        def do_GET(self):
            if self.path in ("/", "/index.html"):
                self.send_response(200); self.send_header("Content-Type", "text/html"); self.end_headers(); self.wfile.write(html)
            else:
                super().do_GET()
    print(f"{len(items)} boards; http://0.0.0.0:{a.port}/", flush=True)
    http.server.ThreadingHTTPServer(("0.0.0.0", a.port), functools.partial(H, directory=str(OUT))).serve_forever()


main()
