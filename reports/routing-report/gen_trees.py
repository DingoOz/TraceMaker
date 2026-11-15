# SPDX-License-Identifier: GPL-3.0-or-later
"""Writes tex/trees.tex: the default run's decision tree as four TikZ figures (from the code map, pipeline.md section 2).

Each node: (id, question, side branch label, side text, down branch label, down text or "", which branch a default run on an
ordinary board takes: "side" or "down", source reference). The main flow runs down the page; the side box is the other branch.
"""
import pathlib

HERE = pathlib.Path(__file__).resolve().parent

TREES = [
    ("tree-prep", "Before routing: reading the board and the rules (steps S1--S12).", [
        ("N1", "--try-layers given?", "yes", "Layer advisor: route each layer count and compare them", "no", "", "down", "main.cpp:1704"),
        ("N2", "--time-total given?", "yes", "One deadline: plain run gets 0.75 T, the later stages share the rest", "no",
         "Every stage gets its own --time (120 s)", "down", "route_job.cpp:249"),
        ("N3", "--reroute given?", "yes", "Unlocked tracks and vias are removed first", "no",
         "All existing copper stays, as fixed obstacles", "down", "route_job.cpp:260"),
        ("N4", ".kicad_pro beside the board?", "no", "KiCad defaults and the rules stored in the board; the warning shows only in --report",
         "yes", "Net classes, minimums and tuning profiles are read", "down", "project_reader.cpp:386"),
        ("N5", "A custom rule cannot be parsed?", "yes", "That rule is ignored with a warning (conditional hole rules are widened board-wide)",
         "no", "", "down", "rule_engine.cpp:536"),
        ("N6", "A clearance rule reads net names?", "yes", "Exact routing: no per-class caches, no cost-to-go fields", "no", "", "down",
         "rule_engine.cpp:586"),
        ("N7", "A net zone has no fill?", "yes", "Filled in memory; the whole run uses soft zones (refill and repair afterwards)", "no",
         "Zone fills are walls", "down", "route_job.cpp:336"),
        ("N8", "--component-rules on?", "yes", "Categories detected: keep-outs, net classes, sidecar rules", "no", "", "down",
         "route_job.cpp:485"),
        ("N9", "Plane, >1 layer, audit not off?", "yes", "Critical nets found, for the audit after the route only", "no", "", "down",
         "route_job.cpp:736"),
        ("N10", "Project defines tuning profiles?", "yes", "Route at the profile width, narrow afterwards", "no", "", "down",
         "route_job.cpp:666"),
        ("N11", "Knowledge base knows failures here?", "yes", "Those connections go first in every variant", "no", "", "down",
         "route_job.cpp:426"),
    ]),
    ("tree-portfolio", "The portfolio and one variant's set-up (steps S13--S17).", [
        ("N12", "More than one variant?", "no", "One router (--variants 1 or --threads 1)", "yes",
         "Portfolio of 8 (= --threads, at most 8)", "down", "route_job.cpp:893"),
        ("N13", "Looks like a BGA breakout?", "yes", "Breakout list of variants, 4 rounds (2 layers, BGA grid, >= 70 % of nets on it)", "no",
         "The designed eight, one round", "down", "router.cpp:7545"),
        ("N14", "--halving and variants > threads?", "yes", "Successive halving", "no", "All 8 variants run at once", "down",
         "router.cpp:7679"),
        ("N15", "A GPU is listed?", "yes", "Variants spread over the GPUs (results identical)", "no", "Cost fields on the CPU", "side",
         "router.cpp:7663"),
        ("N16", "Breakout board (this variant)?", "yes", "Transactions on, direction penalty 0.3, package penalty 3", "no",
         "No transactions", "down", "router.cpp:4565"),
        ("N17", "Variant 3 or 6?", "yes", "Escape plan: corridors reserved for dense-package pins", "no", "No escape plan", "down",
         "router.cpp:7579"),
        ("N18", "Window >= 60,000 cells, fields allowed?", "yes", "Cost-to-go field as the A* heuristic", "no", "Octile heuristic",
         "side", "router.cpp:1823"),
        ("N19", "Field >= 500,000 cells and a GPU?", "yes", "Field on the GPU (CPU if the GPU call fails)", "no", "Field on the CPU",
         "down", "router.cpp:1443"),
        ("N20", "After 10 s, fields > 30 % of time?", "yes", "Fields off for this variant", "no", "", "down", "router.cpp:1831"),
    ]),
    ("tree-connection", "One connection inside a variant, and the passes and restarts (steps S18--S20).", [
        ("L0", "Already joined by copper?", "yes", "Counted as routed (e.g. through a plane via)", "no", "", "down", "router.cpp:6999"),
        ("L4", "Strict search finds a legal path?", "yes", "Commit after the exact geometric check: routed", "no",
         "Windows 2 and 6 mm (pass 0), then 20 mm and the whole board", "side", "router.cpp:2700"),
        ("L5", "Boxed in, or exact check said no?", "yes", "Forced pad escapes, then neck-down width and via, strict", "no", "", "down",
         "router.cpp:7038"),
        ("N22", "Pass > 0 and rip-up on?", "no", "Fails for now: retried in the next pass", "yes",
         "Negotiated search may cross and rip other nets' copper", "down", "router.cpp:7064"),
        ("N23", "Negotiation finds it boxed in?", "yes", "After escapes and neck-down: marked dead, given up", "no",
         "Fails: next pass, most-failed first", "down", "router.cpp:7084"),
        ("N24", "Passes end? (none left, budget, 40)", "no", "Next pass over the failed connections", "yes", "", "down",
         "router.cpp:6975"),
        ("N25", "Budget left and not complete?", "yes", "Restart: rip all, hardest first (6, then 40 diversified)", "no", "", "down",
         "router.cpp:6910"),
        ("N26", "Another variant done at T; now > 2T+5 s?", "yes", "This variant stops", "no", "", "down", "router.cpp:234"),
        ("N27", "Board equals the best state?", "yes", "Clean-up: fewer vias, LNS, smoothing, length tuning", "no",
         "Best state returned without clean-up (unless length rules)", "side", "router.cpp:7310"),
    ]),
    ("tree-after", "After the portfolio: rescue, repair, refill, write and explain (steps S21--S28).", [
        ("N28", "Connections still open?", "no", "Skip rescue, advisor and repair", "yes", "", "side", "route_job.cpp:960"),
        ("N29", "Warm relax rescue routes more?", "yes", "Adopt it (portfolio of 8 at 60 % and 80 % clearance, then legalise)", "no",
         "Keep the plain result", "side", "route_job.cpp:1004"),
        ("N29b", "Still open? Fresh rescue routes more?", "yes", "Adopt it", "no", "Keep the result before", "down", "route_job.cpp:971"),
        ("N30", "Input had unlocked copper and open > 0?", "yes", "Repair windows of 1, 2, 4 mm lift that copper", "no", "", "down",
         "route_job.cpp:1161"),
        ("N31", "Soft zones on?", "yes", "Refill; unconnected? route again; starved thermals repaired", "no", "", "down",
         "route_job.cpp:1218"),
        ("N32", "After-route passes asked for?", "yes", "Octilinear, widen power, stitching (each a transaction)", "no",
         "Only tuning-profile narrowing, if profiles exist", "down", "route_job.cpp:1427"),
        ("W", "Write the board", "", "", "", "Only new tracks and vias added; every other byte kept", "down", "route_job.cpp:1670"),
        ("N33", "Open pins and why-open on?", "yes", "Escape proof per pin: walled in, neck, fills only, congestion", "no", "", "side",
         "route_job.cpp:1801"),
        ("N34", "The time limit ended a search?", "yes", "Warning: the result depends on machine speed", "no", "", "side",
         "route_job.cpp:1982"),
    ]),
]


def esc(s):
    return (s.replace("\\", "\\textbackslash{}").replace("_", "\\_").replace("%", "\\%").replace("&", "\\&").replace("#", "\\#")
            .replace(">=", "$\\geq$").replace("--", "-{}-"))


def tree(key, caption, nodes):
    out = [r"\begin{figure}[p]", r"\centering", r"\begin{tikzpicture}[font=\footnotesize, node distance=1cm,"
           r" dec/.style={draw=accent, fill=accent!8, diamond, aspect=2.6, text width=3.9cm, align=center, inner sep=0pt},"
           r" act/.style={draw=black!45, fill=black!3, rounded corners=2pt, text width=5.3cm, align=left, inner sep=4pt, minimum height=8mm},"
           r" ref/.style={font=\scriptsize\ttfamily, text=black!50},"
           r" arr/.style={-{Stealth}, draw=black!45, thick}, def/.style={-{Stealth}, draw=okgreen, line width=1.8pt},"
           r" lab/.style={font=\scriptsize\bfseries, fill=white, inner sep=1pt}]"]
    y = 0.0
    prev = None
    for i, (nid, q, sl, st, dl, dt, default, ref) in enumerate(nodes):
        if nid == "W":
            out.append(rf"\node[act, fill=okgreen!12, draw=okgreen, text width=3.6cm, align=center] (n{i}) at (0,{y:.2f}) {{\textbf{{{esc(q)}}}}};")
            out.append(rf"\node[act, right=1.4cm of n{i}] (s{i}) {{{esc(dt)}}};")
            out.append(rf"\draw[arr] (n{i}) -- (s{i});")
        else:
            out.append(rf"\node[dec] (n{i}) at (0,{y:.2f}) {{\textbf{{{nid}}} {esc(q)}}};")
            out.append(rf"\node[act, right=1.4cm of n{i}] (s{i}) {{{esc(st)}}};")
            out.append(rf"\draw[{'def' if default == 'side' else 'arr'}] (n{i}) -- node[lab, above] {{{sl}}} (s{i});")
            if dt:
                out.append(rf"\node[act, left=0.9cm of n{i}, text width=3.3cm] (d{i}) {{{esc(dt)}}};")
                out.append(rf"\draw[densely dotted, black!40] (n{i}) -- (d{i});")
                out.append(rf"\node[lab, text=black!60] at ($(n{i})!0.5!(d{i})+(0,0.22)$) {{{dl}}};")
        out.append(rf"\node[ref, below right=-1mm and 1.4cm of n{i}.south, anchor=north west] {{{esc(ref)}}};")
        if prev is not None:
            pdefault, plabel = prev
            out.append(rf"\draw[{'def' if pdefault == 'down' else 'arr'}] (n{i-1}) -- node[lab, right] {{{plabel}}} (n{i});")
        prev = (default, dl)
        y -= 2.45
    out.append(r"\end{tikzpicture}")
    out.append(rf"\caption{{{caption} Green arrows: the branch a default run takes on an ordinary two-layer board"
               r" with a project file, filled zones and a GPU. The grey reference is the line of the decision in the code.}")
    out.append(rf"\label{{fig:{key}}}")
    out.append(r"\end{figure}")
    return "\n".join(out)


if __name__ == "__main__":
    (HERE / "tex").mkdir(exist_ok=True)
    (HERE / "tex" / "trees.tex").write_text("\n\n".join(tree(*t) for t in TREES) + "\n")
    print("wrote tex/trees.tex")
