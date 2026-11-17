---
status: current
code: [src/app/route_job.cpp, src/app/main.cpp, src/route/router.cpp, src/route/router.hpp, src/io/kicad/project_reader.cpp, src/learn/knowledge_base.cpp, src/passes/repair.hpp]
reviewed: 2026-11-17
---

# How the default route decides

This page follows one real board through `tracemaker route board.kicad_pcb -o out.kicad_pcb` with no other
options, stage by stage, in the order the code runs them. For each stage it says what happens and why, which
option changes it, where the design is written down (design doc section and decision record), and how to see it
happen: a line of the log, a field of the `--json` summary, a section of the `--report` run report, or an event
of the live viewer ([viewer protocol](../13-viewer-protocol.md)).

The pipeline is two functions. `run_route_job` in [`src/app/route_job.cpp`](../../src/app/route_job.cpp) runs
the job: rules, portfolio, rescue, repairs, write, explanation. `route_portfolio` and `Router::Impl::run` in
[`src/route/router.cpp`](../../src/route/router.cpp) route the board. Option names and defaults are in
[`src/app/main.cpp`](../../src/app/main.cpp) and [`src/route/router.hpp`](../../src/route/router.hpp); the
[command-line reference](../reference/cli.md) lists them all.

## The example run

The board is `tests/boards/plane_smd`, TraceMaker's own test board: four copper layers, filled plane zones,
11 nets, 47 connections, one net class (0.1 mm track, 0.1 mm clearance). The run is recorded in
[`bench/records/m25`](../../bench/records/m25/README.md) (log, JSON summary, run report, command):

```bash
tracemaker route plane_smd.kicad_pcb -o plane_smd.routed.kicad_pcb \
  --json plane_smd.json --report plane_smd.report.md \
  --no-kb --work 3000000 --view --view-port 9951
```

Two options differ from a plain default run, and both make the run repeatable. `--work 3000000` gives every
router 3 M search expansions instead of stopping on `--time 120` (wall time depends on the machine).
`--no-kb` stops the knowledge base from reordering the run with failures of earlier runs. `--json`,
`--report` and `--view` only observe. The same command run twice wrote byte-identical boards.

Result: 41 of 47 connections routed, 223 tracks, 0 vias, 0 DRC errors added, 13.2 s on 8 threads, exit code 3
(connections left open).

## 1. Options and budget

**What.** The `route` command fills `RouterOptions` (one router) and `RouteJob` (the stages around it). A
`--config FILE` of `name = value` lines is read first; the command line wins. Every option given is recorded
in the summary.

**Why.** The default stops on wall time (`--time 120`), which suits a person at a desk. A work budget
(`--work N`, search expansions per router) makes the output the same on any machine and any thread count
(rule 5: budgets by work units). `--time` then only stops a run that would take too long.

**Options.** `--time S` (per routing stage: the plain portfolio, each rescue pass, each advisor round),
`--work N`, `--time-total T` (one deadline for the whole job; the plain run gets `--rescue-share` 0.75 of it),
`--config FILE`, `--seed`.

**Design.** [doc 02](../02-architecture.md) §6.2–6.3 (determinism, budgets); [doc 05](../05-routing.md) §82;
[D47](../decisions/D047.md) (variant set independent of thread count), [D186](../decisions/D186.md)
(`--time-total`), [D86](../decisions/D086.md) (reported time covers the whole job).

**See it.** `/run/options`, `/run/work_budget`, `/run/time_limit_s` and `/time_limited` in the JSON; the
report's *Options and seed* section. In the example: work budget 3,000,000, time limit 120, stopped by the time
limit: no. The live page's `budget` field reads `3000000 work, --time 120 s, 8 threads`.

## 2. Reading the board and the rules

**What.** The `.kicad_pcb` is parsed into a document (kept for writing) and a board model. Rules are layered as
KiCad layers them: KiCad's defaults, legacy rules stored in the board, the project file (`.kicad_pro`: net
classes, minimums, tuning profiles), then custom rules (`.kicad_dru`). A `Default` net class is added when the
project has none. Every existing track and via, locked or not, is fixed copper: the router never rips copper it
did not place.

Three things in this stage change the run without any option:

- A copper zone with a net and **no fill** is filled in memory and the run switches to soft zones (other nets
  may route through fills; the zones are refilled afterwards, stage 13). A board saved with its fills routes
  with the fills as walls.
- **Tuning profiles** in the project (KiCad 10) route their nets at the profile's widest width and narrow them
  afterwards.
- On a board with a plane zone, the **critical nets** (crystals, clocks, differential pairs) are found by
  part role and name, but only for the audit after the route (stage 15).

Custom rules the engine cannot apply are named as warnings before routing (rule 6).

**Why.** The router must route to the same rules KiCad's DRC will judge, read the way KiCad reads them.

**Options.** `--reroute` (strip unlocked copper first), `--soft-zones`, `--no-fill-planes`,
`--copper-layers`, `--no-tracks-on`, `--layer-cost`, `--layer-dir`, `--no-critical-audit`,
`--component-rules` (off by default).

**Design.** [doc 08](../08-kicad-integration.md) §3.1–3.2; [doc 05](../05-routing.md) §27, §33 (custom rules),
§54 (unfilled zones), §58 (tuning profiles), §44 (critical nets); [D108](../decisions/D108.md),
[D151](../decisions/D151.md), [D72](../decisions/D072.md), [D140](../decisions/D140.md),
[D145](../decisions/D145.md), [D117](../decisions/D117.md).

**See it.** Log lines `planes: zone ... has no fill`, `tuning profile ...` and `warning: ...` when they apply;
`/rules` in the JSON (files found, minimums, net classes, custom rules); the report's *Rules* section. In the
example the project file was found, there is no `.kicad_dru`, the board minimums are 0.1 mm clearance and track,
and the one class `Default` holds 11 nets. The report says "The readers reported nothing they could not read."
The zones were saved filled, so there is no `planes:` line: the fills are walls. The log names the critical nets:

```text
critical nets: 2 found (component roles and differential pairs), audited after the route only; ...
```

## 3. Failure memory across runs (the knowledge base)

**What.** The knowledge base (`~/.local/share/tracemaker/kb.sqlite`) is opened by default. Connections that
failed before on a board with the same features go to the front of every variant's order. With fewer variants
than eight, it also picks which variants to run (a bandit over earlier wins). After the run it records this
run's failures and winner.

**Why.** Tier T3 of the failure memory: a hard connection routed early has more room.

**Options.** `--kb FILE`, `--no-kb`.

**Design.** [doc 06](../06-failure-learning.md) §4; [D6](../decisions/D006.md).

**See it.** The log line `knowledge base: N connections that failed before are routed first`. The example used
`--no-kb`, so there is none. Without it, a second default run on the same board starts differently from the
first: one reason a default run is not repeatable.

## 4. The portfolio

**What.** A default run is a portfolio: up to eight differently tuned routers race, one per thread, each on its
own copy of the board. The size is `--variants` if given, else 8 with `--work`, else `--threads` (8), at most
8. The designed eight:

| # | Name in the log | What differs |
|--:|---|---|
| 0 | exact bends, shortest first | nine direction states per lattice point (exact turn costs) |
| 1 | fast bends, shortest first, crowded source | one state per point; strict searches start from the more crowded end |
| 2 | fast bends, longest first | longest connections first; pitch ×2 on lattices of 3 M points or more |
| 3 | fast bends, jittered order, escape plan | shortest first with seeded jitter; escape corridors reserved |
| 4 | exact bends, jittered order | as 3 with another seed, exact bends, no escape plan |
| 5 | fast bends, cheap vias, crowded source | via cost ×0.4; pitch ×2 on large lattices, ×0.5 on small ones (under 1 M points) |
| 6 | fast bends, cheap crossings, escape plan, crowded source | crossing routed copper costs ×0.5 in negotiation |
| 7 | fast bends, dear vias, crowded source | via cost ×2.5 |

A two-layer board that is mostly one ball-grid breakout gets a different list instead: the breakout profile and
seeded variants, in rounds that warm-start from the best of the round before.

In wall-clock mode, the first variant to route everything at time *T* gives the others until 2*T* + 5 s. With
`--work` there is no shared deadline. Only the first variant streams to the viewer.

**Why.** No one setting wins on every board; on this board three variants tie for first and two of the others
are five connections behind. Running them side by side costs threads, not wall time.

**Options.** `--threads`, `--variants` (above 8 adds seeded variants, up to 64), `--halving`, `--seed`,
`--escape-all`, `--breakout-portfolio`, `--no-fine-variant`.

**Design.** [doc 05](../05-routing.md) §12, §22, §25, §41, §55; [D47](../decisions/D047.md),
[D33](../decisions/D033.md), [D16](../decisions/D016.md), [D63](../decisions/D063.md),
[D101](../decisions/D101.md), [D143](../decisions/D143.md).

**See it.** `portfolio: 8 variants on 8 threads` and one line per variant in the log; `/variants` in the JSON;
the report's portfolio table. The example:

```text
  variant 0 exact bends, shortest first    routed 39 in 2.4 s
  variant 1 fast bends, shortest first, crowded source routed 41 in 3.7 s
  variant 2 fast bends, longest first (2x pitch on large boards) routed 36 in 3.3 s
  variant 3 fast bends, jittered order, escape plan routed 40 in 3.3 s
  variant 4 exact bends, jittered order    routed 36 in 2.6 s
  variant 5 fast bends, cheap vias (2x pitch on large boards, half pitch on small ones), crowded source routed 41 in 4.0 s  <- best
  variant 6 fast bends, cheap crossings, escape plan, crowded source routed 40 in 3.6 s
  variant 7 fast bends, dear vias, crowded source routed 41 in 3.7 s
```

Stages 5 to 10 happen inside each variant.

## 5. The lattice

**What.** Each variant builds its obstacle model (fixed copper, fills, keep-outs, the rule engine) and a
routing lattice. The automatic pitch is the smallest (track width + clearance) over the net classes, divided by
six, rounded down to 5 µm, held between 25 and 100 µm. The lattice is only where the search looks: every path
becomes tracks and vias and is checked exactly before it is committed (stage 14).

**Why.** Six steps per track pitch let lattice tracks pass between fine-pitch pads; integer lattice costs keep
the search exact and deterministic.

**Options.** `--pitch-um`; the variants' scales (stage 4).

**Design.** [doc 05](../05-routing.md) §2, §25; [D4](../decisions/D004.md), [D16](../decisions/D016.md),
[D63](../decisions/D063.md).

**See it.** `pitch` in the summary line and `/pitch_mm` in the JSON (the kept variant's). In the example
(0.1 + 0.1) mm / 6 gives 0.030 mm. Variant 5 halves it on this small board to the 0.025 mm floor, and variant 5
was kept, so the log says `pitch 0.025 mm`.

## 6. The connection plan and the net order

**What.** Pads already joined by copper are grouped (union-find), and each net's groups are joined by a minimum
spanning tree. An edge's length is the copper gap between the pads, with a tiny share of the centre distance to
break ties; a pad may also connect into a fill of its own net. Then the order: shortest first (variant 2:
longest first; 3 and 4: shortest first with seeded jitter), then the knowledge base's earlier failures to the
front, then critical nets and `--first-nets`, then buses.

**Why.** Short connections first leave the long ones the open board; planning by copper gap stops a pin from
being joined across a foreign pin to a farther one.

**Options.** `--no-plan-gap`, `--first-nets`, `--first-nets-ordered`, `--critical-first`, `--bus`,
`--buses`, `--only-net`, `--skip-net`.

**Design.** [doc 05](../05-routing.md) §17, §49; [D54](../decisions/D054.md), [D128](../decisions/D128.md).

**See it.** `/connections` (47 here) and `/line_crossings` (38: how often the straight lines of the planned
connections cross) in the JSON; the `ratsnest` events in the viewer.

## 7. The global plan

**What.** None, by default. The global router (coarse tiles, corridors, a congestion map) runs only with
`--global` or `--global-congestion`. A default run goes straight from the connection plan to detailed search.

**Why.** Measured on the benchmark sets, the corridors did not route more than the portfolio without them, so
the global router stays off.

**Options.** `--global`, `--cut-report`.

**Design.** [doc 05](../05-routing.md) §4, §13, §16, §19; [D53](../decisions/D053.md),
[D56](../decisions/D056.md).

**See it.** Nothing in a default run. With `--global`, `/over_cuts` and `/tightest_cut` appear in the summary.

## 8. Escapes from dense packages

**What.** Variants 3 and 6 plan escapes: for the pins of dense packages (8 or more pads at 1.3 mm pitch or less)
they reserve a corridor out of the package. Reserved cells are closed to other nets in strict searches and cost
2 mm to cross in negotiated ones. A corridor is released when its connection is routed. Pins that can only leave
off the lattice get forced off-lattice escapes later, on the ladder (stage 9).

**Why.** Without reservations, the first connections routed out of a package take the channels the inner pins
need. In all eight variants, escape planning won on some board tiers and lost on others, so it runs in two.

**Options.** `--escape-plan`, `--escape-all` (all variants), `--escape-flow` (min-cost-flow channels for deep
ball grids), `--via-in-pad`.

**Design.** [doc 05](../05-routing.md) §3, §12, §14; [doc 16](../16-bga-breakout-plan.md);
[D32](../decisions/D032.md), [D33](../decisions/D033.md), [D49](../decisions/D049.md).

**See it.** `escape_plan`, `escape_release` and `escape_dead` events in the viewer; `/escape_corridors` and
`/escape_corridor_mm` in the JSON (of the kept variant). In the example the kept variant 5 plans no escapes
(`/escape_corridors` is 0); the escape-plan variants 3 and 6 routed 40 each.

## 9. Search and costs, one connection at a time

**What.** Inside a variant, connections are routed one after another in the order of stage 6. Each one climbs a
ladder until a rung routes it:

1. Already joined by copper (for example through a plane): counted as routed.
2. Strict A\* search on the lattice: eight directions on every copper layer, vias between layers, other nets'
   copper closed. It tries windows of 2 and 6 mm around the connection (plus a quarter of its length) in the
   first pass, and also 20 mm and the whole board in later strict attempts. A flood-fill reachability check
   runs first when failure is likely, and a two-sided proof stops long strict searches that cannot succeed.
3. If a pin is boxed in or the exact check refuses the path: forced off-lattice pad escapes, then a neck-down to
   the board's minimum track width and a smaller via.
4. From the second pass on: a negotiated search that may cross other nets' routed copper at a cost (stage 10).
5. Still boxed in after negotiated escapes and neck-down: the connection is given up for this restart.

The step costs are integers. A straight step costs the pitch *p*, a diagonal *p*√2, times the layer's cost. A
45° turn adds *p*/2, a 90° turn 2*p*; sharper turns are not allowed. A via costs 3 mm of track. The heuristic is
an exact cost-to-go field over the window when the window has at least 60,000 cells (on the GPU from 500,000
cells, on the CPU below; both give the same numbers), otherwise the octile distance.

**Why.** A\* with an exact field is optimal in its window and expands little beyond the path. Integer costs make
CPU and GPU agree bit for bit (rules 2 and 3). Small windows first keep most searches cheap.

**Options.** `--via-cost-mm`, `--heuristic-weight`, `--no-field`, `--no-gpu`, `--fast-bends` (in every
variant), `--crowded-source`, `--layer-dir`, `--layer-dir-pen`, `--layer-diag-pen`, `--micro-vias`,
`--blind-vias`, `--power-width` (a wide strict search first for supply nets).

**Design.** [doc 05](../05-routing.md) §2, §5, §6, §20, §28, §55, §62, §66, §83; [doc 07](../07-gpu.md);
[D4](../decisions/D004.md), [D9](../decisions/D009.md), [D44](../decisions/D044.md),
[D73](../decisions/D073.md), [D143](../decisions/D143.md), [D154](../decisions/D154.md),
[D158](../decisions/D158.md), [D189](../decisions/D189.md).

**See it.** Per variant, on standard error:

```text
searches: 74 ok (966377 expansions), 102 failed (1114685 expansions); fields 28 GPU + 112 CPU (1.43 s, 0 GPU fallbacks, 0 cache hits)
reachability checks: 49, 36 proved unreachable, 1164397 cells visited
```

`stats`, `track_add`, `via_add`, `frontier` and `heatmap` events in the viewer; `/work_all_variants` in the JSON
splits the work by phase (first pass, negotiation, clean-up). The example's 49,305,727 expansions over all
variants and passes are in the log's last `work` line. The connection `INT: U1.21 -> U2.B2` shows the ladder:
"boxed in; escapes/neck-down: skipped: identical earlier attempt failed (nogood); negotiated: boxed in (target);
given up: boxed in on the routing lattice, even with routed copper negotiable".

## 10. Rip-up, negotiation and failure memory within the run

**What.** The first pass is strict. Connections that fail go round again, the most-failed first, for up to 40
passes. From the second pass a negotiated search may cross other nets' routed copper: crossing costs 1 mm plus
four times the cell's history, and committing the path rips the crossed connections, which go back into the
queue (PathFinder-style negotiated congestion). Each conflict adds to a history cost on its cells (2 pitches per
recorded conflict), which every later search pays there. An attempt that failed in exactly the same situation (same window, mode and width) is a
*nogood* and is skipped. When passes stop gaining, the router restarts: it rips everything and routes the
hardest connections first, keeping history and nogoods (6 planned restarts, then up to 40 diversified ones while
budget is left). A variant returns its best state, not its last.

**Why.** Rip-up lets a connection take room another one holds when the other has somewhere else to go. History
makes contested cells dearer until the nets sort themselves out. Nogoods stop the router repeating a search
whose answer it already knows.

**Options.** `--no-rip-up`, `--max-passes` (40), `--max-restarts` (6).

**Design.** [doc 05](../05-routing.md) §7, §17, §41; [doc 06](../06-failure-learning.md) §3.1, §3.3, §3.6;
[D6](../decisions/D006.md), [D99](../decisions/D099.md) (transactions, used on breakout boards).

**See it.** Per variant: `restarts 0; legality checks 2806693; rips 36, passes 2, boxed-in 54, nogood skips 26,
history cells 382, transactions 0 kept of 0`. In the viewer, `track_remove` events (rips), `failure` events and a
`stage` event `restart N`. In the JSON, `/stuck/unrouted_by_reason` and each open connection's cause. Five of the
example's six open connections are `ripped_not_rerouted`, for example "GND: U2.B1 -> U2.C3 (ripped up by SCK and
not routed again)". With 3 M work per variant the budget ran out within one or two passes, before any restart.

## 11. Clean-up inside each variant

**What.** When a variant ends in its best state, it cleans up: each connection is routed again with vias ten
times dearer and kept if cheaper (up to 4 rounds), a neighbourhood search over vias, octilinear smoothing (each
change exactly checked), then length, skew and delay tuning for nets with such rules. A variant that ended away
from its best state returns that state without clean-up, unless the board has length, skew or delay rules.

**Why.** The search optimises one connection at a time on the board as it was then; clean-up revisits each one
on the finished board.

**Options.** `--no-optimize`.

**Design.** [doc 05](../05-routing.md) §8, §74; [D173](../decisions/D173.md).

**See it.** `clean-up: N connections improved` per variant; `/style` in the JSON. In the example the variants
that ended in their best state report 34 and 37 improved; the others report 0, because they ended in negotiation
away from their best state.

## 12. Choosing the result, and the relax rescue

**What.** The portfolio keeps the variant that routed the most connections; ties go to fewer unfinished bus
lanes, then fewer vias, then shorter copper. If connections are still open, the job routes again with *relax,
route, legalise*, in two passes: **warm** (starting from the plain result's routes), then, if still open,
**fresh** (from nothing). Each pass is a portfolio of eight that first routes under lowered clearance (60 %, then
80 % of the board's), then moves the copper off the lattice until it meets the real rules (the legaliser, with
the realizer for the last hand-over), and keeps only what is legal under the board's own rules. A pass is kept
only if its legal result is better than the result before, so the rescue never makes a board worse. With
`--work`, a pass gets rounds of at most 20 M expansions adding up to `--work`; without it, rounds of 20 M until
the board is complete, 200 rounds, or `--time` again.

**Why.** Room that strict clearances hide can be found under relaxed ones and then made legal. A complete board
never gets here. Because each pass has its own `--time`, an incomplete board can take about three times
`--time`.

**Options.** `--no-rescue`, `--rescue-mode warm|fresh|both`, `--relax` (relax from the start, then no rescue),
`--time-total`, `--rescue-share`.

**Design.** [doc 05](../05-routing.md) §41.3, §68, §70, §71; [D104](../decisions/D104.md),
[D160](../decisions/D160.md), [D162](../decisions/D162.md), [D163](../decisions/D163.md).

**See it.** Log lines `rescue (warm): ...`, `ramp stage 1/2 (clearance -0.0400 mm): 43 of 47 routed`,
`warm start: 39 saved routes taken over, 4 not legal under these rules`; `stage` events `rescue` begin and end;
`/rescue` in the JSON; the report's *Relax rescue* section. In the example variants 1, 5 and 7 tie at 41 and variant 5
wins the tie-break (its result has no vias at all). Each rescue pass had one round of 3 M work, so only the first relaxed
stage (clearance lowered by 0.04 mm) ran before legalising:

```text
rescue (warm): 41 -> 39 routed (no gain: the result before stands)
rescue (fresh): 41 -> 39 routed (no gain: the result before stands)
```

## 13. Repair

**What.** Two repairs, each run only when the board needs it, each kept only if fewer connections stay open.

- **Repair windows**: when the input had unlocked tracks or vias and connections are open, the unlocked copper
  within 1, 2 and 4 mm of each open connection is lifted and the area routed again (3 M work per attempt).
- **Refill repair**: when the run used soft zones, the zones are refilled for the new copper as KiCad would fill
  them; items left unconnected are routed again with the new fills as fixed copper, and thermal reliefs the new
  copper starved are repaired pad by pad.

Before them, an advisor is consulted only with `--advisor` (off), and the knowledge base records the run.

**Why.** The router never rips input copper (stage 2); a partly routed board can still need some of it moved.
Soft zones trust stale fills during routing, so the refill is where their connectivity is proved.

**Options.** `--repair auto|on|off`, `--repair-window-mm`, `--repair-work`, `--soft-zones`,
`--no-refill-repair`, `--advisor`.

**Design.** [doc 05](../05-routing.md) §36, §36.1, §75; [doc 17](../17-llm-advisor.md);
[D172](../decisions/D172.md), [D85](../decisions/D085.md), [D137](../decisions/D137.md).

**See it.** Log lines `repair: ...` and `soft zones: after refilling ...`; `stage` events `repair`; `/repair` and
`/refill` in the JSON; the report's *Zone refill* section. The example has no input copper and no soft zones, so
neither ran; the report says "The zones were not refilled by the router".

## 14. After-route passes and the exact check

**What.** The after-route passes are off by default: `--octilinear` (0/45/90° doglegs), `--widen-power`,
`--stitch-vias`. The one that runs without an option narrows tuning-profile nets to each layer's width. Each pass
is a transaction, kept only if no net gets more unconnected items and no tuned net gets worse.

No copper reaches the result without the exact geometric check: every committed path, every clean-up change and
every legalised route is checked against the exact rules, never the lattice (rule 1). The GPU never makes that
decision. With `--report`, the job also runs its own DRC on the board before and after (zones refilled first) and
counts the errors added.

**Why.** Correctness before speed: the lattice and the GPU only propose; the exact check decides.

**Options.** `--octilinear`, `--widen-power`, `--power-width`, `--stitch-vias`, `--report`.

**Design.** [doc 05](../05-routing.md) §5.4, §39, §58, §79, §80; [doc 03](../03-data-structures.md) §6;
[D5](../decisions/D005.md), [D92](../decisions/D092.md), [D145](../decisions/D145.md),
[D187](../decisions/D187.md), [D136](../decisions/D136.md).

**See it.** `exact check rejected the lattice path` as a cause in the log when the check refuses a path;
`legality checks N` per variant (2,806,693 in one example variant); `/drc` in the JSON and the report's *DRC
before and after*. The example: `report: DRC before and after (zones refilled): 0 routing errors added`; errors
0 before and after, unconnected items 47 before, 6 after.

## 15. Writing the board, and the audit

**What.** Only the new tracks and vias are added to the document; every other byte of the input is written back
unchanged (rule 8), and the project file is copied next to the output. Existing fills are not refilled; KiCad
does that. On a board with a plane, the critical nets of stage 2 are then audited: vias, length, crossings of
plane voids, and layer changes without a nearby return via.

**Why.** A designer's file must come back as it went in, apart from the routing.

**Options.** `-o`, `--emit-items` (for the KiCad plugin), `--no-critical-audit`, `--return-paths`.

**Design.** [doc 08](../08-kicad-integration.md) §3; [doc 05](../05-routing.md) §42, §44, §81;
[D7](../decisions/D007.md), [D114](../decisions/D114.md), [D188](../decisions/D188.md).

**See it.** `tracemaker check-roundtrip` proves the round trip on any board. The example:
`critical nets after the route: 2 nets, 0 vias, 0 plane void crossings, 0 layer changes without a return stitch`;
`/critical_nets` in the JSON (XIN and XOUT, the crystal's nets); the report's *Signal-integrity audit*.

## 16. Why connections are open, and the summary

**What.** For every pin of an open connection (up to 400), the why-open proof floods the free space round the
pin for a track of the net's width, against the fixed copper only. A pin no track or via can leave is *walled
in*: no router connects it as placed. A pin only a narrower track can leave needs a neck-down; a pin walled in
only by zone fills needs `--soft-zones`; the rest are *open through congestion*, where more search or more
layers can help. The log ends with `next:` lines built from these classes. Exit code 0 means every connection
is routed (with soft zones: nothing unconnected after the refill), 3 means some are open.

**Why.** An open connection should come with a reason the designer can act on, and a design change (a part
moved, a rule, more layers) is suggested, never made.

**Options.** `--no-why-open`, `--json`, `--report`, `--try-layers 2,4`.

**Design.** [doc 05](../05-routing.md) §55, §69; [doc 20](../20-complete-routing.md) §2.4–2.5;
[D161](../decisions/D161.md), [D143](../decisions/D143.md), [D136](../decisions/D136.md).

**See it.** `/why_open` and `/stuck` in the JSON; the report's *What is open and why*. The example:

```text
routed 41/47 connections, 223 tracks, 0 vias, pitch 0.025 mm, 3110969 expansions, 13.18 s (best variant 3.98 s)
why open: 11 pins of open connections; 0 walled in by fixed copper (0 of them with a via in the pad), 0 only for the class's track or via, 1 only by zone fills or copper graphics, 10 open through congestion (0.19 s)
  next: route again with --soft-zones: 1 pin is walled in only by zone fills
  next: 10 pins are open through congestion: more search (--work, --variants) or more copper layers (--try-layers 2,4 routes each count and compares them)
```

The pin walled in only by fills is `U2.B2` of net INT, the connection the ladder gave up on in stage 9.

## Summary

| Stage | Options | Design doc | Decisions | Where you see it |
|---|---|---|---|---|
| 1. Options and budget | `--time`, `--work`, `--time-total`, `--config` | doc 02 §6; doc 05 §82 | D47, D86, D186 | `/run`, `/time_limited`; report *Options and seed* |
| 2. Board and rules | `--soft-zones`, `--no-fill-planes`, `--reroute`, `--no-critical-audit` | doc 08 §3.1–3.2; doc 05 §27, §54, §58 | D72, D108, D140, D145, D151 | `planes:`, `tuning profile`, `warning:` lines; `/rules`; report *Rules* |
| 3. Knowledge base | `--kb`, `--no-kb` | doc 06 §4 | D6 | `knowledge base: ...` line |
| 4. Portfolio | `--threads`, `--variants`, `--escape-all`, `--seed` | doc 05 §12, §22, §25, §55 | D16, D33, D47, D63, D101, D143 | `variant N ...` lines; `/variants` |
| 5. Lattice | `--pitch-um` | doc 05 §2, §25 | D4, D16, D63 | `pitch` in the summary line; `/pitch_mm` |
| 6. Plan and order | `--no-plan-gap`, `--first-nets`, `--bus` | doc 05 §17, §49 | D54, D128 | `/connections`, `/line_crossings`; `ratsnest` events |
| 7. Global plan (off) | `--global` | doc 05 §4, §13, §19 | D53, D56 | nothing by default; `/over_cuts` with `--global` |
| 8. Escapes | `--escape-plan`, `--escape-all`, `--escape-flow` | doc 05 §3, §12, §14 | D32, D33, D49 | `escape_plan`, `escape_dead` events; `/escape_corridors` |
| 9. Search and costs | `--via-cost-mm`, `--heuristic-weight`, `--no-field`, `--no-gpu`, `--fast-bends` | doc 05 §5, §6, §20, §28, §62, §66; doc 07 | D4, D9, D44, D73, D154, D158, D189 | `searches:`, `reachability checks:` lines; `/work_all_variants` |
| 10. Rip-up and failure memory | `--no-rip-up`, `--max-passes`, `--max-restarts` | doc 05 §7; doc 06 §3.1, §3.3, §3.6 | D6, D99 | `rips, passes, ... nogood skips` line; `failure`, `track_remove` events; `/stuck` |
| 11. Clean-up | `--no-optimize` | doc 05 §8, §74 | D173 | `clean-up:` lines; `/style` |
| 12. Result and relax rescue | `--no-rescue`, `--rescue-mode`, `--relax` | doc 05 §41.3, §68, §70, §71 | D104, D160, D162, D163 | `rescue (warm):`, `rescue (fresh):` lines; `rescue` stage events; `/rescue` |
| 13. Repair | `--repair`, `--soft-zones`, `--no-refill-repair` | doc 05 §36, §75 | D85, D137, D172 | `repair:`, `soft zones:` lines; `/repair`, `/refill` |
| 14. After-route passes, exact check | `--octilinear`, `--widen-power`, `--stitch-vias`, `--report` | doc 05 §5.4, §39, §80; doc 03 §6 | D5, D92, D136, D187 | `legality checks` per variant; `/drc`; report *DRC before and after* |
| 15. Write and audit | `-o`, `--no-critical-audit` | doc 08 §3; doc 05 §44, §81 | D7, D114, D188 | `critical nets after the route:` line; `/critical_nets` |
| 16. Why open, summary | `--no-why-open`, `--json`, `--report` | doc 05 §69; doc 20 §2.4–2.5 | D136, D143, D161 | `why open:`, `next:` lines; `/why_open`, `/stuck`; exit code |

## Where the design docs and the code differ

Read against the code for this page (commit `9b77616`); kept here until the docs or the code change.

- **The ladder.** [Doc 05](../05-routing.md) §6 describes rungs R0–R7. R0 (a global corridor) needs `--global`,
  R3 exists only as the job's repair windows (stage 13), R4 and R5 were not built (§20), R6 is `--cut-report`.
  The ladder that runs is the one in stage 9.
- **The search.** §5.2 describes bidirectional A\* on a radix heap; the code is one-directional A\* on a binary
  heap, with a short reverse search to detect a boxed-in target.
- **Parallelism.** §5.3 routes connections with disjoint windows in parallel; the code routes the connections of
  a variant one after another and runs variants in parallel.
- **The cost.** §5.1 writes the step cost as a product of factors; the code adds integer terms (stage 9).
- **The pass stall test** uses the variant's total rip count: after any rip, a pass that gains nothing does not
  end the passes, which then run on to `--max-passes` or the budget.
- **Unreadable rules.** Rule 6 says never to route on rules that could not be read. A custom rule whose condition
  cannot be parsed is named in a warning and not applied; a missing project file is named only in the run
  report.
- **`--record` with `--view`**: the recording is skipped without a warning.
