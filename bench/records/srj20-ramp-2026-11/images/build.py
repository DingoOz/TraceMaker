#!/usr/bin/env python3
"""Writes index.html: the 11 srj20 boards still open, each with its best board, close-ups and what a person would do."""
import json, pathlib

HERE = pathlib.Path(__file__).resolve().parent
S = json.load(open(HERE / "summary.json"))
CHART = (HERE / "chart.svg.txt").read_text().replace('x="62" ', 'x="70" ')

VIAS = {  # vias in the best board: total, inside the BGA outline, on a dog-bone site (diagonal centre between four balls)
    "sample152": (318, 79, 5), "sample040": (344, 76, 4), "sample070": (302, 62, 1), "sample064": (352, 70, 5),
    "sample176": (324, 79, 9), "sample120": (312, 70, 3), "sample008": (324, 74, 7), "sample088": (324, 71, 4),
    "sample144": (342, 82, 8), "sample200": (342, 60, 3), "sample032": (360, 75, 6)}

ADVICE = {
    "sample152": [
        "<b>bga_conn_061</b> is on the outer ring (west side) and its test point is on the top edge. A person takes it one pitch west on F.Cu, drops a via there and runs up the west side on B.Cu. That spot is taken now: three other nets put their vias right against the BGA's west edge (close-up). Move those vias 0.5 mm further out and this one has room.",
        "<b>bga_conn_055</b> sits two rings in on the north side and goes to the bottom edge. Its four via sites are taken. Give it a dog-bone via on its south-west diagonal, then run straight down on B.Cu under the BGA. The pads are on F.Cu only, so B.Cu under the package is open except where vias stand.",
    ],
    "sample040": [
        "<b>bga_conn_080</b> (ring 1, south side) goes to the west edge, 18 mm away, the longest open connection on the board. It has been open in all 18 saved results. In the close-up, all four of its via sites hold other nets' traces or vias. A person routes this one <i>first</i>: dog-bone via on its south-west diagonal, B.Cu west along a row lane, then up the west side to the test point.",
        "<b>bga_conn_070</b> is three rings in on the south side and goes to the east edge. Same fix: reserve its dog-bone via before the neighbours route through the sites, then B.Cu east between the via rows.",
    ],
    "sample070": [
        "Both open connections go to the opposite side of the board. <b>bga_conn_036</b> is on the outer ring (east side) and goes to the west edge. A person can't cross the BGA on F.Cu (the pads are there), so: via just outside the east edge, B.Cu straight west along a ball row under the package, back up to F.Cu at the west strip.",
        "<b>bga_conn_047</b> (ring 1, north side) goes to the bottom edge: via on its dog-bone site, B.Cu straight down a ball column. Only 1 of the board's 62 vias inside the BGA sits on a dog-bone site. The rest are scattered and cut both the row and column lanes, so neither straight run exists now.",
    ],
    "sample120": [
        "Two of the five are outer-ring balls (<b>047</b>, <b>094</b>). An outer ball normally escapes first, but here the band just outside the BGA is full of vias from inner balls. Keep a via-free band about 1 mm wide round the package for the outer two rings.",
        "<b>094</b> has been open in all 18 saved results. It runs from the south side to the east edge, 18.6 mm, and needs a corner: B.Cu east under the BGA, then F.Cu up the east side.",
    ],
    "sample064": [
        "Eight open, three of them three rings in. Fan out the whole BGA first (every used ball gets its dog-bone via in the same direction per quadrant), then route the long cross-board connections (15-18 mm) before the short ones.",
    ],
    "sample176": [
        "Six of the nine go to the opposite side of the board, and most are two rings in. They need straight B.Cu lanes under the package, which only a regular fanout leaves. Only 9 of the 79 vias inside the BGA are on dog-bone sites.",
    ],
    "sample008": [
        "Half of the 14 open are outer-ring balls, and 10 of 14 go to the opposite side. The outer ring fails because the ring outside the BGA is full of other nets' vias. Keep that band free, and route the long cross-board ones first.",
    ],
    "sample088": [
        "10 of the 17 open balls are on the outer ring: the clearest case of the band round the BGA filling with vias. 11 go to the opposite side. This board has 106 connections, the second-most crossings of all 200 boards, and 324 vias (3 per connection).",
    ],
    "sample144": [
        "Two open balls are deep inside (rings 5 and 6), so they need lanes through the whole fanout. The rest are outer-ring or long. Fan out first, reserving a lane from each deep ball to the edge of the package.",
    ],
    "sample200": [
        "12 of 14 go to the opposite side. One of them (<b>041</b>) is short, 8.8 mm, but its four via sites are taken. The board needs layer discipline: horizontal runs on one layer, vertical on the other, one via per connection. It has 342 vias.",
    ],
    "sample032": [
        "This board has the most crossings in the set (1,431 for 108 connections), and 18 are open, scattered across all rings. A two-layer route is only plausible with the full human method: regular fanout, one layer per direction, one or two vias per connection. It may need a third layer.",
    ],
}

ORDER = ["sample152", "sample040", "sample070", "sample120", "sample064", "sample176", "sample008", "sample144", "sample200", "sample088", "sample032"]


def card(b):
    v = S[b]
    t, ins, site = VIAS[b]
    imgs = "".join(f'<figure><img src="img/{p}" alt="{b} close-up {p}" loading="lazy"><figcaption>{"ball end" if p.endswith("a.jpg") else "test-point end"}, '
                   f'{v["open_conns"][int(p[-6])]["net"]}</figcaption></figure>' for p in v["imgs"])
    rows = "".join(f'<tr><td>{c["net"].replace("bga_conn_", "")}</td><td>{c["ring"]}</td><td>{c["free"]} of 4</td><td>{c["len"]}</td></tr>'
                   for c in v["open_conns"])
    adv = "".join(f"<p>{a}</p>" for a in ADVICE[b])
    return f"""
<section class="board" id="{b}">
  <header><h2>{b}</h2><span class="pill">{v['open']} open</span>
    <span class="meta">{v['conns']} connections · {v['crossings']} crossings · {t} vias ({ins} inside the BGA, {site} on dog-bone sites) · best: {v['best']}</span></header>
  <div class="grid">
    <figure class="full"><img src="img/{b}_full.jpg" alt="{b} best board, open connections dashed" loading="lazy"><figcaption>Best board. Red F.Cu, blue B.Cu, white vias; dashed magenta = open connection.</figcaption></figure>
    <div class="side">
      <div class="advice">{adv}</div>
      <div class="tablewrap"><table><thead><tr><th>Open</th><th>Ring</th><th>Via sites free</th><th>mm</th></tr></thead><tbody>{rows}</tbody></table></div>
    </div>
  </div>
  <div class="zooms">{imgs}</div>
</section>"""


html = f"""<title>srj20 Open Boards</title>
<meta name="claude:viewport-fit-sides" content="cover">
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=IBM+Plex+Sans:wght@400;600&family=IBM+Plex+Mono:wght@400;500&family=Space+Grotesk:wght@500;700&display=swap">
<style>
/* Layout: a lab notebook; a summary of the human method, the crossing chart, then one section per board (image left, notes right). */
:root {{ --bg:#f5f6f8; --panel:#ffffff; --fg:#141821; --muted:#5a6272; --line:#d9dde4; --accent:#b8327a;
  --ok:#2f8f5b; --warn:#c98a10; --bad:#c2352f;
  --display:"Space Grotesk",system-ui,sans-serif; --body:"IBM Plex Sans",system-ui,sans-serif; --mono:"IBM Plex Mono",ui-monospace,monospace; }}
@media (prefers-color-scheme: dark) {{ :root:not([data-theme="light"]) {{ --bg:#0b0e14; --panel:#131824; --fg:#e6e9ef; --muted:#97a0b2; --line:#283044; --accent:#ec6fb3; --ok:#4fbf83; --warn:#e3a93a; --bad:#ef6a62; color-scheme:dark }} }}
:root[data-theme="dark"] {{ --bg:#0b0e14; --panel:#131824; --fg:#e6e9ef; --muted:#97a0b2; --line:#283044; --accent:#ec6fb3; --ok:#4fbf83; --warn:#e3a93a; --bad:#ef6a62; color-scheme:dark }}
body {{ background:var(--bg); color:var(--fg); font:15px/1.55 var(--body); padding-left:max(16px,env(safe-area-inset-left,0px)); padding-right:max(16px,env(safe-area-inset-right,0px)); padding-block:24px 64px }}
main {{ max-width:1180px; margin:0 auto; display:flex; flex-direction:column; gap:28px }}
h1,h2,h3 {{ font-family:var(--display); text-wrap:balance; margin:0 }}
h1 {{ font-size:2rem }} h2 {{ font-size:1.35rem }} h3 {{ font-size:1.05rem; margin-bottom:6px }}
p {{ margin:0 0 10px; max-width:72ch }}
.lede {{ color:var(--muted); max-width:80ch }}
.panel {{ background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:18px 20px }}
.method {{ display:grid; grid-template-columns:repeat(auto-fit,minmax(250px,1fr)); gap:14px }}
.method div {{ min-width:0 }}
.method b {{ font-family:var(--display) }}
.chart svg {{ width:100%; height:auto; max-width:760px; display:block }}
.chart .grid {{ stroke:var(--line); stroke-width:1 }}
.chart .tick,.chart .lab {{ fill:var(--muted); font:12px var(--mono) }}
.chart .lab {{ fill:var(--fg) }}
.board {{ background:var(--panel); border:1px solid var(--line); border-radius:10px; padding:18px 20px; display:flex; flex-direction:column; gap:14px }}
.board header {{ display:flex; flex-wrap:wrap; align-items:baseline; gap:8px 12px }}
.pill {{ font:600 12px var(--mono); color:var(--bad); border:1px solid var(--bad); border-radius:999px; padding:1px 9px }}
.meta {{ color:var(--muted); font:13px var(--mono); flex-basis:100%; min-width:0 }}
.grid {{ display:grid; grid-template-columns:minmax(0,1fr) minmax(0,1fr); gap:18px; align-items:start }}
@media (max-width:820px) {{ .grid {{ grid-template-columns:1fr }} }}
figure {{ margin:0 }} img {{ max-width:100%; display:block; border-radius:6px; border:1px solid var(--line) }}
figcaption {{ color:var(--muted); font-size:12.5px; margin-top:4px }}
.side {{ display:flex; flex-direction:column; gap:12px; min-width:0 }}
.advice b {{ font-family:var(--mono); font-weight:500; color:var(--accent) }}
.tablewrap {{ overflow-x:auto }}
table {{ border-collapse:collapse; font:13px var(--mono); font-variant-numeric:tabular-nums }}
th,td {{ text-align:right; padding:3px 12px 3px 0; border-bottom:1px solid var(--line) }} th {{ color:var(--muted); font-weight:500 }}
th:first-child,td:first-child {{ text-align:left }}
.zooms {{ display:grid; grid-template-columns:repeat(auto-fill,minmax(200px,1fr)); gap:12px }}
nav {{ display:flex; flex-wrap:wrap; gap:6px 14px; font:13px var(--mono) }} nav a {{ color:var(--accent) }}
a:focus-visible {{ outline:2px solid var(--accent); outline-offset:2px }}
</style>
<main>
<header>
  <h1>Why 11 srj20 boards don't close</h1>
  <p class="lede">The best routing so far for each srj20 board that has never been routed to 100%. Every board is one 13 × 13 (or 11 × 11) BGA at 0.8 mm pitch, wired to test points in strips along the edges, on two layers: track 0.12 mm, clearance 0.1 mm, via 0.3 mm. Images and numbers come from the saved boards in bench/results; the suggestions are what a person laying out the board would do.</p>
</header>

<section class="panel">
  <h3>What the 11 boards have in common</h3>
  <div class="method">
    <div><b>No room for a via at the ball.</b> Every open ball has all four of its dog-bone sites (the diagonal gap between it and its neighbours) taken by other nets' copper: none of the four is free for 95 of the 105 open balls, and the other 10 have one or two.</div>
    <div><b>Vias everywhere else.</b> 300–360 vias per board, 3–4 per connection. Of the 60–80 vias inside each BGA, only 1–9 sit on a dog-bone site; the rest block both a row and a column lane.</div>
    <div><b>The open ones are long and cross the board.</b> The open connections run 9–21 mm, mostly to the opposite or an adjacent side. 38 of the 105 open balls are on the <i>outer</i> ring, boxed in by vias crowded against the package.</div>
    <div><b>What a person does.</b> Fan out the BGA first in a fixed pattern and reserve the sites. Keep a via-free band round the package. Run one direction per layer, with one or two vias per connection. Route the long cross-board connections first.</div>
  </div>
</section>

<section class="panel chart">
  <h3>The open boards are the ones with the most crossings</h3>
  <p class="lede">Each dot is one srj20 board: how often its straight ball-to-test-point lines cross one another. That's a lower bound on how much the routes must cross, and two layers pay for every crossing with layer changes. The 11 open boards rank 1–9, 11 and 16 of 200.</p>
  {CHART}
</section>

<nav aria-label="Boards">{''.join(f'<a href="#{b}">{b} ({S[b]["open"]})</a>' for b in ORDER)}</nav>
{''.join(card(b) for b in ORDER)}
</main>
"""
(HERE / "index.html").write_text(html)
print("wrote index.html", len(html))
