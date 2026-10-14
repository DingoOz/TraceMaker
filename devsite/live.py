#!/usr/bin/env python3
"""Live view of one bench/run.py run: every board of its set, with state (queued / running / done).

Usage: devsite/live.py RUN SET [--port 8769]
Reads bench/results/RUN/boards/*.route.json (written when a board finishes) and `ps` for boards in flight.
"""
import argparse, glob, html, http.server, json, os, subprocess, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def board_list(set_name):
    out = []
    for line in open(os.path.join(ROOT, "bench", "sets", set_name + ".txt")):
        line = line.strip()
        if line and not line.startswith("#"):
            out.append(line)
    return out


def running_boards():
    ps = subprocess.run(["ps", "-eo", "etimes,args"], capture_output=True, text=True).stdout
    res = {}
    for ln in ps.splitlines():
        if " route " in ln and "/boards/" in ln or " route " in ln and "srj20" in ln:
            parts = ln.split()
            for p in parts:
                if "/srj" in p and "sample" in p:
                    res[p.split("/")[-2]] = int(parts[0])
    return res


def page(run, set_name):
    boards = board_list(set_name)
    run_dirs = [os.path.join(ROOT, "bench", "results", r, "boards") for r in run.split(",")]  # several runs: later ones win
    live = running_boards()
    rows, done, solved = [], 0, 0
    for b in boards:
        rjs = [os.path.join(d, b + ".kicad_pcb.route.json") for d in run_dirs]
        rjs = [x for x in rjs if os.path.exists(x)]
        rj = rjs[-1] if rjs else ""
        if rj:
            d = json.load(open(rj))
            c, r = d.get("connections", 0), d.get("routed", 0)
            done += 1
            solved += (c == r)
            state = "SOLVED" if c == r else "open %d/%d" % (c - r, c)
            rows.append((b, "ok" if c == r else "part", state, "%d/%d" % (r, c), "%.0f s" % d.get("seconds", 0)))
        elif b in live:
            rows.append((b, "run", "running", "", "%d s" % live[b]))
        else:
            rows.append((b, "q", "queued", "", ""))
    body = "".join("<tr class=%s><td>%s<td>%s<td>%s<td>%s" % (c, html.escape(b), html.escape(s), r, t)
                   for b, c, s, r, t in rows)
    return ("<meta http-equiv=refresh content=30><title>%s</title><style>body{font:14px sans-serif;margin:20px}"
            "td{padding:2px 12px}.ok{background:#cfc}.part{background:#fec}.run{background:#cdf}.q{color:#888}</style>"
            "<h2>%s (%s)</h2><p>%d boards, %d finished, %d complete, %d running. %s</p><table>"
            "<tr><th>board<th>state<th>routed<th>time%s</table>" %
            (run, run, set_name, len(boards), done, solved, len(live), time.strftime("%H:%M:%S"), body))


ap = argparse.ArgumentParser()
ap.add_argument("run"); ap.add_argument("set"); ap.add_argument("--port", type=int, default=8769)
a = ap.parse_args()


class H(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        b = page(a.run, a.set).encode()
        self.send_response(200); self.send_header("Content-Type", "text/html"); self.end_headers(); self.wfile.write(b)
    def log_message(self, *x): pass


http.server.ThreadingHTTPServer(("0.0.0.0", a.port), H).serve_forever()
