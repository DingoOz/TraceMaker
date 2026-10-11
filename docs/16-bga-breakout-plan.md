# 16 — Plan: matching tscircuit on BGA breakout boards

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Written 2026-10-11.
>
> **Status: plan only. Nothing here is built.** Where it says "TraceMaker will", read it as a proposal. The
> evidence is in [`../research/bga-routing-2026-10.md`](../research/bga-routing-2026-10.md); evidence tags
> [V] verified, [A] abstract only, [C] calculated are used the same way here. Proposed milestone: **M14**.
> This note changes no code, so it adds no row to [12-decisions.md](12-decisions.md); the decisions it
> proposes (§7) get their rows when they are made.

## 1. Goal and scoreboard

**Goal.** Route at least as many of tscircuit's BGA boards as tscircuit's router does, judged by KiCad's DRC
(and, as a second score, tscircuit's own relaxed checker), without adding a DRC error to any board.

| Track | Set | tscircuit (dashboard run `38105514608-1`, commit `65522c8`, 2026-10-11, 1x effort) | TraceMaker today (doc 10 §7, 50 M work, one variant, one thread) | Gate |
|---|---|---|---|---|
| A | `srj19` (200 BGA breakouts, 2 layers, MIT) | 92.0 % completed, 90.0 % relaxed pass (184 / 180 boards) | 79.5 % clean (159 boards) | **≥ 184 clean** and ≥ 180 relaxed |
| A | `srj20` (200 BGAs with missing balls, 2 layers, no licence: local use only) | 78.5 % / 78.5 % (157 boards) | 50.0 % clean (100 boards) | **≥ 157 clean** and ≥ 157 relaxed |
| B | `fanout31` (72 SoC fan-out cases, 8-layer fixtures) | 48 / 72 solved (committed report 2026-09-09, 120 s per case) | not run; no importer for the boundary-port semantics | **≥ 48 / 72** (same cases, all balls validated) |

Rules for the comparison:

- **Pin the opponent.** tscircuit's rates moved from 91.5 / 77 to 92.0 / 78.5 within a day. Every quoted figure names a
  dashboard run id; `bench/` stores the per-board JSON of the pinned run so results can be joined board by board.
- **Matched effort, stated.** tscircuit's setting is 360 s per board on a 32-vCPU runner (median 19–23 s). Report two
  rows: TraceMaker at the current standard budgets, and at ≤ 360 s wall on ≤ 32 threads (the portfolio is allowed;
  it is how this router uses cores).
- **Tune / hold out.** Tune on samples 1–100 of each set, report on all 200, and keep 101–200 untouched until a
  change is final (CLAUDE.md's held-out rule; the sets are public and small enough to overfit).
- **Never lose a board.** Three srj19 boards (24, 54, 126) are clean in TraceMaker and fail tscircuit's check; keep
  them. A change that drops a currently clean board is a regression (rule 9).

## 2. What exists (and where)

The SRJ importer/exporter, the two benchmark sets and the scorer (D90) live on branch `t6-srj` (commit `c00c71a`),
and bus routing (D91) and after-route passes (D92) on `srj-bus-postroute`; **none of it is on `main`**. M14.0 below
merges it. The router, escape corridors (`--escape-plan`), the flow planner (`--escape-flow`, doc 05 §14), the
escape feasibility analysis (`tracemaker escape`), differential pairs (D50) and zone-aware routing (§26) are on `main`.

## 3. What the data says

All figures: the 400 boards of srj19/srj20, TraceMaker at 50 M work from `build/t6-srj/bench/results/*-w50m`,
tscircuit from the pinned dashboard run. Computed in this session [C].

**3.1 The gap is a cliff in board size, not a general weakness.**

| Connections per board | srj19: boards | TM | tscircuit | srj20: boards | TM | tscircuit |
|---|--:|--:|--:|--:|--:|--:|
| < 20 | 26 | 100 % | 100 % | 25 | 100 % | 100 % |
| 20–39 | 94 | 100 % | 99 % | 74 | 92 % | 100 % |
| 40–59 | 60 | 63 % | 82 % | 57 | 12 % | 88 % |
| 60–79 | 17 | 6 % | 71 % | 30 | 0 % | 23 % |
| ≥ 80 | 3 | 0 % | 0 % | 14 | 0 % | 7 % |

By straight-line crossings between a board's connections (a proxy for how many escapes must swap sides): below 100
crossings TraceMaker passes all 103 srj19 boards and all 64 srj20 boards (tscircuit: 102 and 64); at 100 or more
crossings TraceMaker passes 58 % (56 of 97 boards, srj19) and 26 % (36 of 136, srj20) against tscircuit's 80 % and
68 %. The boards TraceMaker loses and tscircuit wins have a median of 331 (srj19) and 289 (srj20) crossings.

**3.2 The misses are not near-misses.** Of the 81 boards that tscircuit solves and TraceMaker does not, 32 leave
more than ten connections open, 24 leave six to ten, and only 11 leave one or two. A last-few-connections tool
(exact window solve, group re-route: doc 05 §17 found no effect) cannot close most of the gap.

**3.3 Per-ball escape is not the limit.** `tracemaker escape` reports every ball escapable on every board checked
(srj19 samples 11, 19, 31, 117 fail in both routers); `escape --flow` plans samples 19 and 117 entirely on the pad
layer. A ball can leave alone and the set of balls still cannot be completed: the connections have **fixed
destinations** (I/O pads around the package, passives on the other side), so the escapes must also arrive in a
compatible order, and the unordered flow model ignores that (research L2).

**3.4 More search does not close it, and the escape plans do not help.** srj19 stops improving between 50 M and
200 M (88 % on its first 50 boards); the eight-variant portfolio at 10 M equals one variant at 50 M; `--escape-plan`
and `--escape-flow` lower both sets' clean rate (66 → 60 % and 34 → 32 / 28 %). srj20 is still rising with budget
(46 → 56 % at 200 M), so part of that set is budget-limited.

**3.5 tscircuit's router is topology first.** Its stages: escape-via location → topology planning → a capacity-mesh
global path per connection (1.8–2.2 s of ~70–105 s) → per-cell high-density routing (37–41 % of time) → *force
improve* (27–36 %) → DRC repair (12–21 %). Most of its failures (8 of 20, 35 of 43) are timeouts inside force
improve. It uses **more vias**: 72 per board against TraceMaker's 44 on the 156 srj19 boards both solve (57 against 40
on the 100 srj20 boards both solve; 66 % and 43 % more) [C].

**3.6 Geometry.** At 0.8 mm pitch a channel holds one track, and a through via at an interstice closes the two
channels it touches on both layers (research §A). On two layers every escape is a channel or a via site; they compete.

## 4. Hypotheses and the experiments that test them

None of the explanations below is established. The first milestone runs the cheap experiments, and the build
list in §5 is ordered by what they find. A hypothesis is dropped when its experiment fails its stated test.

| # | Hypothesis | Experiment | Supported if / dropped if |
|---|---|---|---|
| H1 | **Crossings are resolved by search order, not by a plan.** The sequential router commits early escapes that make later crossings impossible (3.1, 3.2, 3.3) | **E1, topology oracle.** Run tscircuit's router locally (already fetched in `bench/data/tscircuit/tscircuit-autorouter`) on the 81 gap boards; from its output extract each connection's layer sequence and via positions; give TraceMaker those vias as fixed waypoints (soft cost bonus around them) and route | Supported if TraceMaker then completes ≥ 70 % of the 81 boards. Dropped if it completes < 30 %: the limit is then detailed search or geometry, not topology |
| H2 | **Under-use of vias at the package.** tscircuit uses 43–66 % more vias on the same boards (3.5); our cost model makes vias too dear near a dense package | **E2.** Scale the via cost to a fraction (`1/2`, `1/4`) inside the package footprint + 2 mm on the 81 gap boards, one variant, 50 M | Supported if clean rate on the gap boards rises ≥ 10 points with no tune-set board lost. Dropped if flat or lower |
| H3 | **The lattice wastes channel width.** A track on a lattice node cannot sit centred in a 0.44 mm channel for 0.15 mm + 2 × 0.10 mm; the half-pitch last-gasp rung does not cover whole fan-outs | **E3.** Re-run the gap boards with the lattice pitch halved (budget scaled) and with channel-centred pad-escape stubs (`pad_cells`) | Supported if clean rate rises ≥ 10 points at equal expansions. Dropped otherwise |
| H4 | **srj20 is partly a budget limit.** Its curve still rises at 200 M (3.4), and 35 of tscircuit's 43 failures are timeouts | **E4.** srj20 at 200 M and 800 M, eight variants, on the 56-core machine, wall time recorded | Supported if the rate reaches ≥ 65 % with no structural change. Tells how much of the srj20 gap is plain compute |
| H5 | **Part of the unsolved boards is infeasible.** 17 srj19 and 43 srj20 boards defeat both routers | **E5.** A joint (not per-ball) cut bound: demand versus capacity across the package edge per layer, as R6 does for boards (doc 05 §20), using the exact gap capacities of `escape_flow` | Gives the denominator for "feasible boards", as `clean_pass_feasible` does for PCBench. A proof of infeasibility on a board removes it from the target, with the cut printed |

E1 is the deciding experiment. It separates "TraceMaker needs a topology planner" from "TraceMaker's detailed
search cannot realise a good topology", which need different work. It uses tscircuit's code only as a measurement
instrument on public data; nothing of it enters the engine.

## 5. Features to add, in order

Order follows what E1–E5 are expected to show; reorder once they have run. Each item lands with its reference path
and equivalence test (rule 3), integer fixed-point costs and fixed tie-breaks (rule 2), and is an option or portfolio
arm until the quick tier and the srj tune half show no regression (rule 9).

### F0. Tooling (M14.0; no router change)

1. Merge `t6-srj` and `srj-bus-postroute` into `main`. Quick tier must stay byte-identical (D90 reports it so).
2. `bench/srj_vs_tscircuit.py`: join a TraceMaker run with a stored dashboard run JSON (`data/runs/<id>.json`, one
   file kept under `bench/data/tscircuit/dashboard/`, not committed) and print the 2×2 table, the gap boards, and
   per-bucket rates as in §3.
3. Failure classifier in `bench/run.py` output: per open connection, "no channel / no via site / ripped too often /
   budget" from the router's existing reason strings, so progress can be attributed.
4. A `crossings` column in the board JSON (the proxy of §3.1), so a new board's difficulty is known before routing.

### F1. Package-scale breakout planner (the main build; depends on E1 supporting H1)

A planner for **ordered** escape on the pad-gap graph, run before the search and used as guidance.

- **Graph.** Nodes: the gaps and interstices between pads (as `escape_flow` already builds them), with capacities
  from exact geometry including the diagonal (research L1); via sites as layer-change edges that remove the
  channels they touch (3.6); a few coarse nodes outside the package towards each destination pad.
- **Problem.** Each connection is a commodity with a fixed source ball and destination pad (research L2). Choose a
  path of (layer, gap) steps. Crossings between paths on one layer are forbidden; a crossing is resolved by giving
  one path a via site and the other layer.
- **Method.** Negotiated congestion (PathFinder: McMurchie and Ebeling, FPGA 1995, already the basis of doc 05)
  over the gap graph with via-site and channel capacities as the shared resources; a one-layer planar subset
  first (Ozdal and Wong's maximal planar subset, research §B) as the initial layer assignment, the rest to the
  other layer.
- **Output as guidance, not reservation.** A cost bonus along the planned path, and a fixed-cost via site per
  connection that needs one; **never** corridors that block other nets. Reservations are the thing that made every
  earlier escape plan lose (M1, M2, M3).
- **Reference and equality.** With guidance disabled the router is byte-identical to today's. A test builds boards where the
  plan is trivial (one ring) and checks the plan equals the old corridors' escape.
- **Kill criterion.** If E1 shows the oracle topology is not enough (H1 dropped), do not build F1; do F2/F3 first.

### F2. Joint via-site assignment (depends on H2)

`escapeViaLocationSolver` is tscircuit's first stage. Match balls that need a via to interstice sites in one
min-cost matching: cost by distance and by how many channels the site closes, hard constraint that a via and a
track through the same interstice cannot coexist (3.6), and a joint choice rather than greedy (tscircuit's own
fix, research T7). Extends the dog-bone corridors of doc 05 §12 from "pointing away from the centre" to a matched
set. If E2 supports H2 with only a via-cost change, F2 is that cost change and stops there.

### F3. Channel-centred pad stubs and a finer last rung (depends on H3)

Escape stubs that centre the track in the channel, and a half-pitch retry of a whole fan-out (not a window)
before the net is declared boxed in. Small, local to `pad_cells` and the escalation rungs.

### F4. Compute plumbing (depends on H4)

Run the portfolio with the cores the machine has and a wall-time budget of 360 s; report `work` and wall separately
(D86). No algorithm change; it fixes how srj20 is reported against a 32-vCPU opponent.

### F5. A joint feasibility certificate (E5; independent)

Extend `--cut-report` to the package: per layer, capacity of the pad-gap graph across a cut around the package
against the number of connections that must cross it, with the over-full cut printed. Also gives infeasibility
proofs without a SAT solver. A SAT/CP window solver (research L4, L7) is **not** proposed: only 11 of the 81 gap
boards are within two connections of done (3.2), and doc 05 §20 already found an exact window model would mostly
answer "feasible, the heuristic just did not find it". Revisit if F1 leaves many near-misses.

### F6. Gridless polish (low priority; depends on H3 and on F1 leaving clearance-limited failures)

tscircuit spends 27–36 % of its time in a force-based improver, and it is where most of its timeouts are. A
TraceMaker equivalent would be a transaction (rule 4) that nudges committed tracks and vias apart inside a
window to open a channel, accepted only if the exact check passes and an open connection then routes. Build only
if E3 shows clearance-limited failures; otherwise it adds work without moving the gate.

### Track B: fan-out of SoC packages (`fanout31`)

A different problem (research §A): a prefix, with boundary exits, plane drops and bus rules, on 4–10 layers.
Needed, in order:

1. **B0 baseline.** Convert the 72 cases from `dataset-fanout31-am62l` with the `srj-import` path (the SRJ in
   `fanout-solver/tests/fixtures/*.json` has `simpleRouteJson`, `solverOptions`, `buses`, `differentialPairs`).
   Add set `fanout31` and report solved / 72 with "every ball validated" as the pass rule, 120 s per case.
2. **B1 boundary ports.** A connection whose second point is on the shared breakout edge: the end may slide along
   the edge, exit order is fixed by the bus, and the route ends exactly on the edge. Model as a row of virtual
   pads on the edge (off-board edge keep-in), not as a new rule.
3. **B2 plane drops.** `termination: plane` is a via to an inner plane layer; doc 05 §26 (plane-aware routing)
   covers the router side. The 102–117 plane balls per case compete with signals for interstice sites, so F2
   (joint via-site assignment) matters most here.
4. **B3 buses and skew.** D91 (buses, skew) and D50 (differential pairs) exist on branches; check that atomic
   bus layer, all-or-none vias, 45° only and the length-skew limits are what they enforce.
5. **B4 capacity.** 8–10 layers at 0.08128 mm tracks: lattice memory and the cost-to-go fields. Measure first.
6. **B5 geometric pre-check.** The BGA400 four-layer wall (research T1) means some cases are unroutable under the
   stated rules; the F5 certificate is the check.

tscircuit solves AM62L 12 / 12, K230 12 / 12, RK3308 9 / 12, i.MX6ULL 11 / 12, T113-S3 4 / 12 and AM3352 0 / 12. All
of AM62L, K230, RK3308 and i.MX6ULL (48 cases) would match the count without T113-S3 or AM3352. Track B is
independent of Track A and should start only after M14.1's findings, because F2 serves both.

## 6. Milestones

| # | Deliverable | Gate |
|---|---|---|
| M14.0 | F0 tooling; `main` holds the SRJ work; tune/hold-out split recorded | Quick tier byte-identical; `srj_vs_tscircuit.py` reproduces §3's tables from the stored run |
| M14.1 | E1–E5 run, results written into this document (§4 filled with measured outcomes) | Each hypothesis marked supported / dropped with the numbers; build list in §5 re-ordered |
| M14.2 | The first of F1 / F2 / F3 that M14.1 supports | Tune half (1–100 of both sets): +10 points of clean boards or the feature is removed; no board lost |
| M14.3 | The rest of the supported features; F4 and F5 | srj19 ≥ 184, srj20 ≥ 157 on all 200 at the matched-effort row; no KiCad DRC error added |
| M14.4 | Track B (B0–B5) | fanout31 ≥ 48 / 72 |

If M14.1 shows that neither a planner nor a cost change moves the tune half, the honest outcome is a report that
says so and a smaller gate (the boards that E5 does not prove infeasible and that stay budget-limited), not a
softened criterion.

## 7. Decisions proposed (to be recorded when made)

1. Planner output is guidance only, never reservation (research M1–M3).
2. tscircuit's code is a measurement tool, not a dependency, and its datasets are fetched, never copied (D90).
3. Exact solvers are used for certificates and small windows, not as the main method (research L7, doc 05 §20).
4. Learned components are not a dependency (research L10; rule: learned arms only through the bandit).

## 8. Risks

| Risk | Mitigation |
|---|---|
| The unsolved boards are infeasible and the gate is unreachable | F5 certificates; the report separates feasible and infeasible boards |
| E1 cannot be run (tscircuit's router or its inputs do not run here) | It is already in `bench/data/tscircuit/tscircuit-autorouter` for the scorer; if node versions block it, hand-extract the topology of a dozen gap boards from the SVG snapshots its repository publishes |
| Overfitting 400 public synthetic boards | Tune/hold-out split; PCBench BGA set (17 boards) and quick tier as regression guards |
| Judges differ (KiCad against tscircuit's relaxed check): they disagree on 1–2 boards of 200 (doc 10 §7) | Report both; do not tune to the checker's collinear-segment quirk |
| tscircuit's rates keep moving | Pin the run id; re-join on a newer run as a separate row |
| srj20 has no licence | Local benchmarking only; nothing copied into the repository (D90) |
| The synthetic boards are two-layer and may not predict real BGA boards | Keep PCBench and the DAC2020 set in the regression gate; Track B adds real SoC packages |

## 9. Open questions

1. Whether the unsolved 17 / 43 boards are infeasible (E5).
2. What tscircuit's force-improve stage does; its write-up was not found, so F6 is a guess from the timing.
3. Whether a 36–54 ball srj board's difficulty comes from the destination order or from the passives on the other
   side (srj19 failures have a median of 42 bottom-layer destinations against 8–9 on successes; srj20 has none on
   the bottom). E1 and the bucket tables should be split by that too.
4. Whether `fanout31`'s fixtures are the cases tscircuit's committed 48 / 72 refers to: the committed report is a
   month older than the latest fixes, so the gate may need re-pinning once B0 runs.
