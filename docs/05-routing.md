# 05 — Routing

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).
> The stage list, cost formulas and acceptance rules of [`../cpp_autorouter_clean_sheet.md`](../cpp_autorouter_clean_sheet.md)
> §5 are adopted as written unless this document changes them. Read that document's §2 for the evidence
> (PathFinder, FGR, BoxRouter, AIR, TritonRoute-WXL, Contour, B-Escape, Yan & Wong, Lin et al.).

## 1. Principles

1. **Optimal search first.** Every single-connection search is an A* (or bidirectional A*) with an
   admissible, consistent heuristic, so it returns the *minimum-cost path under the current cost model*.
   What is not optimal is the joint problem; negotiation and learning (doc 06) close that gap.
2. **Escalate on failure, never repeat blindly.** Each connection climbs an escalation ladder (§6). Each
   failed rung writes a failure record. The next attempt reads them.
3. **Proofs, not timeouts, drive the expensive steps.** A placement ECO or a "placement-limited" verdict
   requires a proof that a cut is over-full or a window is infeasible.
4. **Octilinear, not Manhattan.** OrthoRoute (GPU PathFinder on a Manhattan H/V layer-pair lattice) is fast
   on regular backplanes but reaches only ~1–2% clean pass on mixed boards in PCBWorld. TraceMaker routes
   0/45/90° on every layer, with only a soft preferred-direction cost.
5. **The KiCad DRC is the judge.** Geometry is generated with the compiled KiCad rules and checked
   exactly before commit.

## 2. Representation for detailed routing (revision of clean-sheet §4.3/§5.4)

The clean-sheet design proposed a gridless corner-stitched tile-plane search. This plan changes the
order of implementation:

| Phase | Search space | Why |
|---|---|---|
| First | **Fine octilinear lattice + exact validation + gridless cleanup.** Per layer and clearance class, a bit-packed grid at pitch `p = gcd-friendly fraction of (width + clearance)`, typically 25–50 µm, plus **pin-access points** off-lattice at every pad (TritonRoute-style). Obstacles are rasterised *conservatively* from exact geometry bloated by `width/2 + clearance`. | Simple to make correct, maps onto GPU fields, deterministic, memory is abundant (a 300 × 300 mm, 12-layer board at 25 µm is 1.7 G cells = 216 MB as 1 bit/cell/class) |
| Later (portfolio arm) | **Gridless tile plane** (Contour-style corner stitching) | Better on irregular fine-pitch geometry; added once the lattice router is benchmarked |

Conservative rasterisation can only make the router miss tight gaps, never create violations; the
cleanup stage (§8) recovers the gaps gridlessly, and the last-gasp stage retries at half pitch in a window.
Every committed segment is validated by exact integer geometry against the R-tree before commit.

## 3. Escape (fanout) planning

As clean-sheet §5.2 (min-cost flow with diagonal capacity, Yan & Wong; layer/order from Ozdal & Wong and
Lin et al.; negotiated fallback). Additions:
- **Escape templates from the knowledge base** (doc 06 tier 3): a successful escape pattern for a footprint
  geometry + rule class is reused as the first candidate on later boards.
- Escape solving for each dense part is independent → run in parallel.
- Built so far: version 1, reserved corridors (§12); version 2, the min-cost-flow channel and layer assignment
  above for deep arrays, opt-in (§14). The negotiated-congestion fallback is not built.

## 4. Global routing

As clean-sheet §5.3, with:
- **3-D tile graph** with tile ≈ 8–10 track pitches; capacity per edge from the free cross-section after
  pads and keepouts (exact, from the R-tree), via capacity per tile.
- **Steiner topology**: rectilinear Steiner trees from FLUTE (Flute3, BSD-3, as in OpenROAD `stt`), adjusted
  for 45° segments; re-derived when pins merge. GeoSteiner is not used (CC BY-NC licence).
- **GPU acceleration** (doc 07): pattern routing (L/Z shapes, all layers) for all two-pin connections in one
  batch, then GPU sweep-based maze routing (GAMER-style) for the overflowed ones, in batches of
  connections whose bounding boxes do not overlap. CPU A* is the reference and the fallback.
- **Output**: corridors, preferred via tiles, and an overflow map written into the history map `h`.

## 5. Detailed routing

### 5.1 Cost of a step on layer L

```
cost = len · (1 + w_dir·[against preferred direction]) · (1 + w_h · h(tile)) · corridor_factor
     + bend_cost(angle)            // 45° turn cheap, 90° dearer, 135° forbidden by default
     + via_cost(type, span)        // per via type; micro/blind cost from the stackup
     + present_conflict(owner)     // only when rip-up is allowed: crossing foreign copper
     + learned_penalty(cell)       // targeted history from failure records (doc 06 §3.2)
```

All costs are integer fixed-point (int64), so A* is exact and GPU/CPU agree bit for bit.

### 5.2 Search

- **Bidirectional A*** with the **GPU cost-to-go field** as the heuristic when the window is large:
  the GPU computes an exact lower-bound distance field from the target (multi-layer, via-aware, ignoring
  rippable copper) by parallel Bellman-Ford/sweeps over the window. The field is an admissible
  heuristic that is far tighter than octile distance, so A* expands little beyond the optimal path.
  Small windows use the octile + via lookahead table (VPR map lookahead).
- **Multiple non-dominated labels** per (cell, direction) where the bend cost makes arrival direction
  matter (state = cell × incoming direction × layer).
- **Priority queue**: radix heap (monotone integer keys) — O(1) amortised push, cheap pops.
- **Visited sets**: generation-stamped dense arrays per window (no clearing cost between searches).

### 5.3 Parallelism

Connections whose search windows do not intersect are routed in parallel on the CPU pool; the
scheduler picks windows in a deterministic order. Each search writes into a private transaction; the
results commit in id order.

### 5.4 Insertion

Lattice path → octilinear segments (merge collinear runs) → snap to pad access points → exact clearance
check (both sides of every new pair) → transaction commit. Track widths: net-class width; neckdown only
inside pad escape regions and **never below the board minimum or the narrowest declared width**.

## 6. The escalation ladder (per connection)

Each rung is tried only if the failure memory does not already hold a nogood for this connection's
current region signature at this rung (doc 06 §3.3).

| Rung | Attempt | Optimality | On failure, record |
|---|---|---|---|
| R0 | A* in the global corridor, no rip-up | Optimal in corridor | blocking objects on the frontier boundary |
| R1 | A* in the bounding box ×3 (grown on contact), no rip-up | Optimal in box | same, plus closest approach |
| R2 | Negotiated rip-up search: foreign unfixed copper is crossable at `present_conflict` cost; victims rerouted (clean-sheet §5.6 step 2) | Optimal for the cost | victim set, victims that failed |
| R3 | Repair window: rip everything foreign in bbox + 4 / + 12 pitches; reroute under several orders in sibling transactions | — | orders tried, residual conflicts |
| R4 | **Exact window solve**: multi-commodity flow ILP on the window lattice at 2× coarser pitch (CP-SAT), with boundary terminals fixed | **Proved feasible or infeasible** | infeasibility core: the minimal set of nets whose boundary terminals conflict |
| R5 | Trial ordering of the remaining connections (B-Escape): commit the one that traps the fewest others | — | trap counts |
| R6 | Cut-capacity proof (Maley): find the over-full cut on every layer | Proof | cut geometry + deficit → **placement ECO request** (doc 04 §4) |
| R7 | Mark placement-limited (if ECO is not allowed or failed) and report | — | final reason |

The ladder is **per connection but with memory**: a connection that failed R0–R2 on the last iteration
starts at R2 next time unless the region signature changed.

## 7. Negotiated repair

As clean-sheet §5.5 (aggressor/victim queue, shifted clip windows, reroute caps, failure predictor),
with the DRC markers coming from the own DRC (doc 03 §6) after each batch. History updates go to the
shared history map *and* the failure memory's conflict graph.

## 8. Cleanup

As clean-sheet §5.7: pull-tight, via minimisation by layer reassignment, 45° smoothing, corner reduction,
optional arcs. Each pass is a transaction with work budgets. Also: **teardrops** only if the project enables
them (KiCad 8+ generates teardrops itself; TraceMaker leaves them to KiCad by default).

## 9. Rules covered (phased)

| Phase | Rules |
|---|---|
| 1 | Clearance (all object pairs incl. copper text/graphics), track width, via size/drill/annular ring, hole-to-hole, hole clearance, edge clearance, keepouts and rule areas, net-class rules, `.kicad_dru` clearance/width conditions on net class, layer and area; `disallow` and `physical_hole_clearance` (§27) |
| 2 | Zone connections (thermal reliefs to planes), solder-mask bridge awareness, blind/buried/micro vias |
| 3 | Differential pairs (coupled routing as a single "pair" object in search: §15), length and skew targets (meander tuning in cleanup), max uncoupled length (`diff_pair_uncoupled`) |

## 10. Considered and not chosen as the core

| Approach | Why not the core | Role |
|---|---|---|
| Manhattan lattice PathFinder on GPU (OrthoRoute) | Poor clean pass on mixed boards | GPU global routing borrows the negotiation idea only |
| Pure RL routers (DeepPCB, DreamerV3+FR, PCBWorld baselines) | Commercial or wrapper-based; behaviour not provable; heavy training | Learned *net ordering* and *strategy selection* later (doc 06 tier 3) |
| Gridless "radar scan" (3D LineExplore, 2026) | No public code; promising results | Possible future portfolio arm |
| Hypergraph successive approximation (tscircuit-autorouter) | Tuned for small boards | Reference for capacity-node global routing |
| Topological rubber-band routing | No mature open implementation | Future portfolio arm |

## 11. Implementation status (2026-10-02)

| Part | Status | Where |
|---|---|---|
| Octilinear lattice A* (pitch (width + clearance) / 6, 25–100 µm), optional 9-state bend tracking | Done | `route/router.cpp` (`search`) |
| Exact legality from the DRC rule engine; per-class fixed-obstacle code caches; separate routed-copper index; near-routed count raster to skip far queries | Done | `route/obstacles.cpp`, `router.cpp` |
| Exact verification of every segment and via before commit | Done | `commit` |
| Escape stubs off the lattice for fine-pitch pads; pad legs dropped when the track already ends on the pad | Done | `pad_cells`, `commit` |
| Escalation: forced escapes, neck-down to the board minimum width, negotiation | Done | `run` |
| Boxed-in detection at the source (open list exhausted) and at the target (short reverse probe) | Done | `search_and_commit_inner` |
| Zone (plane) targets; MST connection planning over existing copper clusters | Done | `plan`, `search` |
| Plane-aware routing of all-SMD boards: soft zone fills, untouched planes as via targets, vias kept off small pads, via in pad for inner balls, first nets (2026-10-07) | Done, opt-in (D65–D69). See §26 | `obstacles.cpp`, `plan`, `pad_cells`, `search`, `commit` |
| GPU cost-to-go fields as the heuristic (never used to prune) | Done | `gpu/field_cuda.cu`, `build_field` |
| Portfolio of 8 variants on a thread pool, early stop when one is complete (wall-clock mode), 2x pitch for two variants on large boards; with `--work` all 8 run at any `--threads` and the winner is chosen by a total order ending in the variant index, so the output is bit-identical across thread counts (D47) | Done | `route_portfolio` |
| Global routing (CPU): tile graph (8 pitches), cut-based edge capacities, via capacity per tile, net-shared edges, integer costs, negotiated congestion; corridors (`--global`, `--global-confine`) and congestion map (`--global-congestion`) | Built, off by default: measured three times on hard boards, corridors cost completion and the map is neutral (§13, §16, §19). No GPU version: routing the tile graph takes 0.02–0.5 s | `route/global_router.cpp` |
| Clean-up (section 8): via-saving re-routes, region rip-up around vias, path smoothing | Done | `optimize_vias`, `lns_vias`, `smooth_paths` |
| Escape planning (section 3), version 1 (M9, 2026-10-04) | Partly done: escape corridors (opt-in), feasibility analysis, via neck-down, dead pins. Version 2 (2026-10-05): min-cost-flow channel and layer assignment for deep arrays, opt-in (`--escape-flow`), measured below version 1. Not built: NC fallback, escape templates. See §12, §14 | `route/escape.{hpp,cpp}`, `router.cpp` |
| Differential pairs (version 2, M12): coupled pair search with coupled vias, breakout/fan-in legs, re-coupling after rip-up, enclosed-pin check; length tuning (custom `length` rules) and skew tuning (custom `skew` rules, `--pair-skew-mm`) | Done, opt-in (`--diff-pairs`, D50). Not built: pair twists, pairs ending on routed copper, pair-aware global routing. See §15 | `route/router.cpp` (`route_pair`, `tune_skew`), `route/diff_pair.{hpp,cpp}` |
| Buses (2026-10-11): groups by `--bus NAME=PATTERN` or detection (`--buses`), lanes routed one after the other along the neighbour on the same layers, nets outside do not rip them, skew matching by meanders from a custom `skew` rule or `--bus-skew-mm`; `tracemaker buses` measures | Done, opt-in (D91). See §38 | `route/bus.cpp`, `order_buses`, `route_bus_lane`, `tune_bus_skew` |

## 12. Escape planning, version 1 (M9, 2026-10-04)

**What failed.** On the 17 PCBench boards with BGAs and other dense packages (`bga-base`, 41 % clean), most
unrouted connections were pins reported "boxed in". Two different causes hide behind that word:

1. *Infeasible under the fixture's rules.* PCBench ships boards without their `.kicad_pro`, so KiCad's defaults
   apply (track 0.25 mm, clearance 0.2 mm, via 0.8/0.4 mm), and old boards keep a large `pad_to_mask_clearance`
   with untented vias. The designers' own routed boards (`raw.kicad_pcb`) fail KiCad's DRC under these rules by
   hundreds of errors on OtterCast (clearance, track width, via diameter) and decelerator4030 (clearance). No
   router can route those pins cleanly; Freerouting fails the same boards.
2. *Feasible but taken.* On large boards (logicbone: 1,188 connections, 0.035 mm lattice) the strict first pass
   alone uses the whole 120 s budget; pins whose escape channel another net took first stay boxed in because
   negotiation never starts.

**What was built.**

| Part | What | Result |
|---|---|---|
| Escape corridors (`--escape-plan`; on in two of the eight portfolio variants, D33) | Dense packages (≥ 8 copper pads, pin pitch ≤ 1.3 mm): perimeter pins get a corridor straight out of the package (2 mm past the pad edge; 0.5 / 1 / 2 / 3 mm gave logicbone 961 / 964 / 968 / 977 and decelerator 449 / 479 / 491 / 490 at a fixed budget); inner SMD balls get a dog-bone corridor to the diagonal via site pointing away from the package centre, reserved only where that net's via fits. Band ≤ ½ pitch (≤ 0.35 pitch for dog-bones) so neighbouring corridors never overlap. Other nets may not enter a corridor in strict searches and pay 2× the crossing cost in negotiated ones; a corridor is released when its pin is connected and planned again on every restart. Reservations only remove options, so they cannot create violations | logicbone (one variant, 160 M expansions): 938 → 964 routed, boxed-in 152 → 116; decelerator (200 M): 446 → 454. All 8 variants on, 120 s: BGA set 6,916 → 6,947 routed, tier B 5,163 → 5,171, tier C 9,521 → 9,514, tier A unchanged (100 %), clean pass unchanged everywhere, no added errors. As a portfolio arm (2 of 8 variants): tier B 5,166, tier C 9,526, clean pass unchanged |
| Via neck-down rung | When the class via does not fit, the escalation rung (with the track neck-down) uses the smallest via the board minimums allow (KiCad checks vias against those, not the net class), drill ≥ 0.2 mm | d20_tri (80 M): 164 → 188 routed; OtterCast (60 M): 160 → 177; both with 0 added KiCad DRC errors |
| Dead pins | A connection still boxed in by a negotiated search (which may cross all routed copper) at the neck-down width, neck-down via and with off-lattice escapes is enclosed by fixed copper: it is not retried in later passes or restarts and is reported as such | Same results at a fixed budget (retrying a sealed pocket is cheap); clearer failure reasons |
| Feasibility analysis (`tracemaker escape <board> [--json]`) | Per dense package: breadth-first search from each pin over a 0.04 mm lattice of the package area, fixed copper only, at the neck-down width and via; a pin escapes when it gets 0.5 mm outside the package. Dead pins are explained ("no channel at W mm and no via site within reach", "only the solder-mask rule blocks via sites: untented vias") with a hint (tent vias / reduce `pad_to_mask_clearance`) | 0.1–2 s per board. Across all 1,157 PCBench boards: 708 have dense packages, 42 have pins that cannot escape even with the neck-down via (789 of 46,628 pins; tiers B 2/45, C 4/39: OtterCast, PCIE-to-MXM, sbc, zx-sizif, memsarray, a motor board). sbc: 22 DRAM balls blocked only by the mask rule. `bench/run.py` records `dead_pins` per board and `clean_pass_feasible`; `bench/feasibility.py` splits finished runs (BGA set: 41.2 % clean, 46.7 % on its 15 feasible boards) |

**Tried and dropped.** Routing connections that touch dense-package pins first (then shortest first) in the
strict pass: logicbone 964 → 750, decelerator 479 → 431 (one variant, same budget) — the many short connections
finish first under shortest-first. Second-ring channel corridors: mixed (logicbone 964 → 954, decelerator
479 → 484), kept behind `--escape-second-ring`.

**What limits the feasible large boards now.** logicbone (all 908 dense-package pins can escape) routes 999 of
1,188 connections in 120 s and only 1,005 in 600 s: in 600 s each variant completes just two passes (the negotiated
pass on a 2,754 × 1,847 × 2 lattice at 0.035 mm takes the rest), and the remaining failures are mostly nogood skips
and windows without a path. That is negotiation speed on large lattices (global routing, M6), not escape.

**Not built yet (rest of M9).** ~~Min-cost-flow channel assignment for arrays deeper than two rings (Yan & Wong),
layer assignment per ring~~ (built 2026-10-05, opt-in, §14), escape templates in the knowledge base (doc 06 T3), and completion over escapable
connections (the benchmark now reports a feasible clean pass per board, not per connection).

## 13. Global router v2, first steps (M6, 2026-10-05)

**Where the time goes on a large board.** logicbone, one variant, 160 M expansions: 938 connections routed in one
pass; the 253 failed searches used 124 M expansions (76 %), the 938 successful ones 39 M. Failed strict searches
flood their whole window before giving up, and negotiation (where they would be fixed) never starts.

**Corridor confinement (built, off: `--global-confine`, `--global-corridor-only`).** The global result now keeps
each corridor's tile bounding box. With confinement, a connection's first search runs in a window cropped to that
box with cells outside the corridor blocked; with `--global-corridor-only`, a non-negotiated search that fails in its
corridor goes straight to negotiation instead of trying the wide windows (capped at 200 k expansions).
10 hard boards (`bench/m6_bench.py`: logicbone, decelerator, EEZ, sbc, LimeSDR and five tier D boards), one variant,
100 M expansions each, total connections routed:

| No global routing | v1 (soft corridor cost) | + confinement | + corridor-only |
|---|---|---|---|
| **8,285** | 8,092 | 7,936 | 7,568 |

Every global variant is worse: the coarse corridors (tile capacities from free boundary samples, two-pin
connections, no pin-escape or via demand) are worse guides than the detailed router's own A* with the GPU
cost-to-go field, and confining searches to them costs completion. Deferring to negotiation is worse still at a
fixed budget, because negotiated searches cost about twice as much per connection (rip-ups). Corridors will only
help once the global plan models pin access, via demand and multi-pin topology well enough to be trusted; until then
the throughput problem is attacked in the detailed router.

**Search cap.** Failed searches stop at `max_expansions` (3 M). One variant, fixed budget, the same 10 boards:
3 M / 2 M / 1 M / 750 k / 500 k routed 8,285 / 8,309 / 8,381 / 8,371 / 8,363. The KiCad-judged tiers with the real
setup (8 variants, 120 s) did not confirm it: tier B 5,167 → 5,161 routed (67.5 % clean both), tier C 9,523 → 9,512
and 63.3 % → 60.0 % clean (two boards lost, one gained). The default stays 3 M; fixed-budget single-variant gains
must be checked on the tiers before they become defaults.

**Reachability pre-check (on: `--reach-check 1`).** Before a strict search that is likely to fail (a retry with a
larger window, or a connection that has failed before), a flood fill over the same lattice and the same legality
tests (`cell_cost`, `via_cost_at`; any layer change where blind/buried vias are allowed) but with no bend states,
turn limits or costs. It admits every path the A* could find, so "no path" is exact: the A* and its cost-to-go field
are skipped and the miss is classed as window (the flood touched the window edge) or enclosed. Each lattice point
is visited once (versus up to nine heap-ordered states), the legality cache it fills is reused by the A* when a path
exists, and visits count as work units so `--work` runs stay deterministic. `--reach-verify` runs the A* anyway after
every "no path" and counts paths it finds (integration test `reach_verify`: 0 mismatches on sbc). The same 10 boards,
one variant, 100 M expansions: off / likely failures / every strict search routed 8,285 / **8,332** / 8,336; mode 1
is better or equal on every board, mode 2 swings both ways (logicbone +45, Aleste −30). Checking every strict search
on sbc proves 72 of 102 failed searches unreachable and cuts failed-search expansions from 23.9 M to 0.4 M, but the
flood on the searches that succeed costs as much again. KiCad-judged tiers (8 variants, 120 s): tier B 5,167 → 5,165
routed, tier C 9,523 → 9,523, tier D 16,626 → 16,628, tier A 2,637 → 2,637; clean pass unchanged (100 %, 67.5 %,
63.3 %, 50 %). Neutral within wall-clock noise at 120 s with 8 variants; kept on because it is exact and wins at fixed
budgets on the largest boards.

**Next.** Reusing a failed search's explored region for the next window was not built: the pre-check already makes
the hopeless retries cheap, and the retries that do find a path need a full A* in the larger window anyway (the
smaller window's g-values are not optimal in the larger one). Still open: making the global plan trustworthy (pin access and via
demand in the tile capacities, multi-pin Steiner topology) before it guides anything.

## 14. Escape planning, version 2: min-cost-flow channels and layers (M9, 2026-10-05)

**Formulation** (`route/escape_flow.{hpp,cpp}`, `--escape-flow`, off by default; D49). Only *deep arrays* are
planned this way: SMD pads of one footprint on a square grid (≥ 80 % of the balls within pitch/8 of one grid,
which tolerates a few test or mounting pads; ≥ 5 × 5 points, ≥ 30 % populated, pads larger than the pitch left
to the fixed-copper checks) with a pin to route in the third ring or deeper. Every other dense package keeps
version 1's corridors (§12), so with no deep array the plan is version 1's exactly.

1. *Pad layer* (Yan & Wong, DAC 2009, network-flow escape model with diagonal capacities). Nodes are the square
   gaps between four balls (split into in/out with the gap's capacity), arcs join neighbouring gaps through the
   channel between two balls. Channel capacity = how many tracks fit: k tracks need k·w + (k + 1)·s of free
   gap, w and s the most common class width and clearance among the array's pins (ties: the smaller), gap =
   pitch minus the two pad half-extents across the channel. Gap capacity = the same count across the narrower
   diagonal (√2·pitch, integer square root, minus both pads' diagonal radii); every track turning in or crossing
   a gap passes a diagonal, and taking the narrower one for all is conservative. A missing ball counts as a point
   obstacle. Each channel midpoint and gap centre is also checked against fixed copper with the router's own
   legality test (`fixed_code` at the planning width) and gets capacity 0 if blocked (thermal pads, planes,
   keep-outs). Every ball to route is a unit source joined to its four gaps; gaps outside the array are the sink.
   Min-cost max flow by successive shortest paths (Dijkstra on reduced costs; integer costs 7 per half
   diagonal, 10 per channel step, 5 straight out of a perimeter ball), so as many balls as possible escape
   without a via, by the shortest channel sequences.
2. *Via sites* (dog-bones): the balls left get one of their four interstitial sites by a second min-cost flow
   (one via per site; only sites no pad-layer escape crosses, where the via clears the four balls by the
   clearance and that net's via passes the fixed-copper check; cost 0 for the site pointing away from the
   package centre, 1 sideways, 2 inwards).
3. *Further layers*, nearest to the pad layer first (the layer-by-layer assignment of Ozdal & Wong, TCAD 2006,
   and Lin et al., DAC 2021): the same flow model on a grid shifted by half a pitch, where the obstacles are the
   planned vias (all of them: through vias) and the nodes are the gaps between via sites. Balls that escape there
   are assigned that layer; the others try the next layer.

Flow paths are decomposed in source order with arcs in fixed order; escapes sharing a channel are placed side by
side at the track pitch around the centre of the free gap. The corridor of a pin is its dog-bone (if any) plus the
channel polyline (gap centres and channel points) on its layer, extended 2 mm past the array, reserved exactly as
version 1's (blocked for other nets in strict searches, 2× crossing cost in negotiated ones, released when the
pin connects, re-planned on restarts). Reservations only remove options; every commit is still checked exactly.
`tracemaker escape <board> --flow` prints the assignment per ring (pad layer / via + other layer / via only /
none) and per layer; `route --escape-report` reports per ring how many deep-array pins were connected.

**What the plan finds on the BGA set** (`escape --flow`, the boards' own rules): 8 of the 17 boards have no
deep array (LimeSDR_Sony's BGA-named part is too small to be one; EEZ, d20, VESC, red-scout have none); OtterCast's array
has no channel and no via site under the fixture rules (rings 2–6: 0 of 42 balls); PocketBone (×3) and
DoroidOscillo escape every ball on the pad layer; decelerator needs vias from ring 2; logicbone (2 layers) puts
rings 1–2 and 62 of 92 ring-3 balls on F.Cu and 74 on B.Cu, but 130 balls of rings 3–10 get a via site with no
room left on B.Cu between the vias; sbc and Own-Mailbox's DRAM have no via site for most inner balls (the
solder-mask rule, §12).

**Evaluation** (one variant, `--work N --variants 1 --threads 1 --no-gpu`, routed connections; rings =
connected/pins of rings 1, 2, 3 of the deep arrays):

| Board (budget) | Off | Version 1 (`--escape-plan`) | Version 2 (`--escape-flow`) |
|---|---|---|---|
| logicbone (160 M) | 907 (68/124, 60/114, 25/92) | **931** (71, 62, 25) | 916 (66, 56, 26) |
| decelerator4030 (200 M) | 444 (9/44, 2/32, 1/16) | **462** (10, 1, 2) | 448 (8, 2, 1) |
| sbc (60 M) | 333 (34/64, 14/55, 11/42) | **345** (40, 17, 17) | 340 (40, 16, 13) |
| DoroidOscillo (40 M) | 264 (11/18, 9/14, 4/11) | 265 (12, 9, 4) | **271** (14, 10, 5) |
| Own-Mailbox pierre (40 M) | 363 (27/33, 20/31, 9/21) | **364** (28, 20, 11) | 361 (29, 21, 6) |
| PocketBone (40 M) | 199 (28/30, 20/20, 17/17) | **202** (30, 20, 17) | 200 (29, 20, 16) |
| OtterCast (60 M) | **177** | 167 | 167 (same plan as version 1: no flow escapes) |
| **Total** | 2,687 | **2,736** | 2,703 |

Version 2 beats routing without a plan (+16) but loses to version 1 (−33) on these boards; it wins only on
DoroidOscillo, the one array whose every ball can leave on the pad layer through planned channels. Narrower bands
so perimeter corridors and channel exits never overlap (0.24 and 0.15 pitch) did not change the picture
(logicbone 908 / 917, decelerator 452 / 468, sbc 343, pierre 363). KiCad DRC of the version-2 outputs of
DoroidOscillo, logicbone and PocketBone: 0 added errors. Planning takes milliseconds (it runs again on every
restart).

**Why it does not help yet.** The corridors through the array are long, and reserving them (and their 2 mm
extensions between the perimeter corridors) takes room the perimeter pins' own routes need outside the package:
on logicbone rings 1–2 lose 11 connections while ring 3 gains 1. The router's own A* already finds the channel
an inner ball needs once the perimeter fan-out is in place, so a reservation helps most where it protects a
short, contested exit (version 1's perimeter corridors and dog-bones). The flow plan is kept, opt-in, as the
routability analysis it is (which balls can leave on which layer under the board's rules) and as the starting
point for the next steps: plan-guided rather than reserved corridors (a cost bonus instead of blocking others),
reserving only the inner rings' channels, and an NC fallback on the escape graph for the balls the flow leaves.

## 15. Differential pairs, version 2 (M12, 2026-10-05)

**Before.** `--diff-pairs` (v1) searched the pair's centreline on one layer with a disk wide enough for both tracks,
offset it into two tracks and only then tried straight or dog-leg legs to the pads. On the USB hub and similar boards
it coupled nothing: the legs failed, halves that had to change layer could not, and whatever was coupled was later
ripped by negotiation and re-routed as single tracks.

**What was built** (off by default: `--diff-pairs` for KiCad's pairs by name, `RouterOptions::pair_nets` for pairs
named otherwise, e.g. USB DP/DM from `--component-rules soft`; D50).

| Part | What |
|---|---|
| Pair rule (`route/diff_pair.cpp`, `pair_rule`) | Width and gap, highest source first: a custom rule's `diff_pair_gap` (opt, else min); the net class's diff-pair width, gap and via gap when the project sets them; else the class width and the clearance KiCad requires between the halves (relaxed to the class diff-pair gap only for pairs KiCad recognises by name, as its DRC does). Never below the board minimums. `diff_pair_uncoupled` (max) limits the legs. Offsets carry a 1 µm rounding margin per side, so the gap comes out 2 µm wide of the rule. Coupled vias sit side by side at the via gap (and the mask web of untented vias) |
| Coupled search (`route_pair`) | A* over (layer, lattice point, direction, which half is on the left). Moves: one straight lattice step; a 45° turn followed by K straight steps, K·pitch ≥ 2·offset·tan 22.5° + width so the inner track's miter never folds back; a coupled via pair (both halves jog out at 45° to the via spacing, change layer side by side, jog back; MV steps). Each move is checked exactly (`segment_state`, `via_state` against fixed and routed copper) on both offset tracks, so the coupled section keeps its gap by construction. Cost: length + K·pitch per turn + two vias; heuristic 2 × straight-line distance to the end pads plus a via pair while off the end pads' layers (weighted: legal, not optimal). Budget 600 states per lattice step of the pair's length (40 k–250 k). Work is the states closed plus every exact leg check of a start or goal candidate, legal or not, charged to the router's total every 64 steps, when the work budget and the time limit are also checked, so an attempt whose candidates all fail ends on either (§30, D77) |
| Breakout and fan-in | Start candidates: every lattice point within R of the start pads' midpoint (0.3 × the pair's length, at least 1.5 × the pads' distance, 0.6–2.5 mm), 8 directions, sides by the shorter legs. Legs: straight, the two octilinear dog-legs, or the same to a point behind the end of the coupled section followed by a straight entry; at the pair width, then the neck-down width; checked exactly, against each other and against the other half's first straight run, when the A* pops the candidate. The pads must lie behind the start (ahead of the end), so legs never double back along the pair. Legs cost twice the coupled length, so coupling starts as close to the pads as the board allows. Goal candidates near the end pads wait in their own queue and are checked at least every 16 expansions; the first legal one is taken |
| Commit | Both halves are built from corners only (a node's own offset point would fold an inner miter back), collinear runs merged, checked against each other geometrically, then exactly: each half against the board, the first committed, the second checked against it too, otherwise the first is taken back. Up to four finished candidates per search |
| Enclosed pins | A pin of another net between the two pins of an end (the ground pin between P and N on HDMI parts) is tested with a short strict search before and after the pair; a pair that boxes in a pin that could escape before is taken back |
| Negotiation and clean-up | A coupled half ripped by negotiation takes its partner with it, and the pair is tried coupled again (twice at most) before single routing. In the clean-up, pairs that ended up routed singly are lifted and routed coupled; on failure their old copper is restored exactly, so completion is unchanged. Via optimisation, smoothing and LNS leave coupled connections alone |
| Skew | `--pair-skew-mm X` (or a KiCad custom `skew` rule) meanders the shorter half in the clean-up with the length-tuning code until the halves differ by at most X/2 (meanders go to the free side) |
| Measurement | `tracemaker pairs <board> [--pair A,B] [--json]`: per pair the track length, the coupled share (20 µm samples beside a parallel track of the other half on the same layer within gap + max(gap/4, 50 µm)), the gap kept (median, minimum), skew and vias. `tracemaker route` prints the same for the pairs it routed; `bench/pair_eval.py` routes boards off/on at a fixed budget and judges both with KiCad |

**Results.** 18 PCBench boards with differential pairs by name (USB, Ethernet, HDMI/TMDS, DisplayPort lanes, PCIe,
MDI), one variant, `--work 30000000`, KiCad 10 DRC (`bench/pair_eval.py`):

| Board | Pairs | Coupled share per pair (on) | Gap kept (target) | Skew median off → on | Routed off → on | KiCad added errors off → on |
|---|---|---|---|---|---|---|
| 4-port-usb-hub_4port-usb-hub | 5 | 94 93 84 93 92 | 0.202 (0.200) | 5.82 → 1.06 | 113 → 113 / 113 | 0 → 0 |
| PmodHDMIIn_PmodHDMIIn | 4 | 55 8 41 56 | 0.152 (0.150) | 12.01 → 9.66 | 113 → 112 / 116 | 0 → 0 |
| kitspace_USBee32-S2 | 1 | 70 | 0.202 (0.200) | 1.33 → 0.74 | 159 → 159 / 159 | 0 → 0 |
| EtherCAT_shield_v1_EtherCAT_shield_v1 | 4 | 80 19 78 22 | 0.202 (0.200) | 3.63 → 2.96 | 226 → 230 / 272 | 0 → 0 |
| Omega2-mini-dock_Omega2 mini-dock | 3 | 4 53 39 | 0.202 (0.200) | 0.22 → 0.20 | 76 → 76 / 76 | 0 → 0 |
| USB-Adapter_USB Adapter | 1 | 0 | – (0.150) | 2.42 → 2.42 | 42 → 42 / 42 | 0 → 0 |
| USBtin_USBtin | 1 | 84 | 0.202 (0.200) | 5.55 → 0.88 | 54 → 54 / 54 | 0 → 0 |
| android_debug_cable_android_debug_cable | 1 | 10 | 0.250 (0.200) | 6.40 → 6.40 | 32 → 32 / 32 | 0 → 0 |
| Own-Mailbox-Hardware_eth | 2 | 69 84 | 0.182 (0.180) | 0.00 → 1.38 | 176 → 180 / 294 | 0 → 0 |
| kitspace_OtterPillG | 1 | 0 | – (0.157) | 8.71 → 1.61 | 104 → 103 / 109 | 0 → 0 |
| kitspace_USB-LED-Otter | 1 | 0 | – (0.200) | 0.00 → 0.00 | 33 → 33 / 39 | 0 → 0 |
| ULPI-Pmod_ULPI-Pmod | 1 | 0 | – (0.200) | 0.33 → 0.33 | 54 → 54 / 62 | 0 → 0 |
| edid-injector_edid-injector | 5 | 88 88 88 88 75 | 0.154 (0.152) | 1.09 → 0.03 | 112 → 112 / 112 | 0 → 0 |
| RaspberryPi-PoE_PoELLi_PI | 7 | 0 24 74 55 0 0 0 | 0.201 (0.199) | 3.26 → 2.64 | 84 → 84 / 84 | 27 → 28 |
| kitspace_stack-light | 5 | 0 0 31 48 85 | 0.155 (0.153) | 5.19 → 6.50 | 301 → 303 / 306 | 0 → 0 |
| kitspace_CH330 | 1 | 0 | – (0.200) | 0.00 → 0.00 | 22 → 22 / 24 | 0 → 0 |
| HY-AI7688H-RevA_HY-AI7688H | 8 | 57 82 76 85 91 71 71 68 | 0.252 (0.250) | 2.01 → 1.32 | 375 → 375 / 375 | 0 → 0 |
| kitspace_USB-C-Screen-Adapter-LDR6023SS | 8 | 91 63 51 87 66 0 0 0 | 0.202 (0.200) | 0.44 → 0.22 | 125 → 126 / 134 | 0 → 0 |

59 pairs (some are not signal pairs: KiCad's naming also pairs `POE_V1+/-` or `AG_IN_+/-`). Coupled share ≥ 80 % on
18, ≥ 50 % on 35, some coupling on 45; median 57 %. Where coupled, the gap is the rule's gap plus the 2 µm rounding
margin everywhere. Intra-pair skew (track length) fell on most boards without any tuning (hub median 5.8 → 1.1 mm); with
`--pair-skew-mm 0.5` the hub's five pairs end at 0.07–0.26 mm, still 82–92 % coupled and KiCad-clean. Completion
with pairs on is equal on 11 boards, higher on 5 and lower by one connection on 2 (PmodHDMIIn 113 → 112,
OtterPillG 104 → 103); at a fixed budget single connections swing both ways with any change (PmodHDMIIn off routes
105 / 106 / 113 at 20 / 25 / 35 M, on 105 / 107 / 112; OtterPillG off 104 / 104 / 108 at 25 / 30 / 35 M): 2,201 → 2,210
routed in all. KiCad finds no error on routed copper with pairs on except on RaspberryPi-PoE, which has 27 with pairs
off as well: all are clearance to graphics on the Margin layer, which KiCad treats as board edge and the router's
obstacle model does not read (one more of them lies on a pair track). Pairs left uncoupled: the halves would have to
swap sides between the ends (ULPI-Pmod, android_debug_cable: the pin order is mirrored), a pair that changes layer
from a bottom-side connector (CH330, USB-LED-Otter) found no via site, short pairs under 1.5 mm (USB-Adapter), and
pairs ripped by negotiation that could not be coupled again in the clean-up. With pairs off the output is
byte-identical to the previous router (six boards checked).

**Tried and dropped.** Making coupled copper four times dearer to cross in negotiated searches (pairs kept their
coupling a little more often, but PmodHDMIIn routed 110 instead of 112 of 116); holding back a tenth of the work
budget for the clean-up re-coupling (removed without a separate measurement: on a board that uses its whole budget the tenth
is taken from routing, which matters more there than coupling); refusing every pair with a pin between its pins (safe, but HDMI connectors whose
ground pins escape on their own lost 88 % coupling for nothing; the before/after test replaced it).

**Not built** (twists and per-pair skew limits were added on 2026-10-06, §24). Pairs whose pin order is mirrored between the ends need a twist (one half crosses the other through a
via): such pairs (ULPI-Pmod, android_debug_cable) were routed singly. A pair ends on pads only, so a pair joining copper
already routed for its nets (a T at an AC-coupling capacitor or termination) often fails its goal legs. No pair-aware
global routing; no rounded or arc corners; skew in picoseconds needs the stackup (doc 15 §5.3); tuning meanders on
one half reduce coupling locally (both halves meandering together is not built); per-pair skew limits from the
component-rule catalogue are not wired (one global `--pair-skew-mm`).

## 16. Global congestion map (M6, 2026-10-06)

**Why the boards fail.** Unrouted connections of the non-clean boards in the last full tier run (`reach1`, 8 variants,
120 s): of about 960 on tiers B–D, roughly 600 are pins boxed in by nearby copper (with or without a nogood skip), 137
are "no path in window" and 171 were never attempted before the time limit; the last two kinds sit on a few large
tier D boards (Aleste alone has 204 unrouted). The nearly clean boards, which decide the clean pass, are almost all
boxed-in pins. Global routing addresses the second group only.

**What was built** (off: `--global-congestion`, `--global-congestion-from`, `--global-congestion-pen`). The output §4
names and v1 never had: the global plan as a map. `GlobalResult::util` holds, per tile and layer, the planned tracks
through the tile's four boundaries in eighths of their capacity (integers). The detailed router adds
`(util − from) × pen` pitches per lattice step in tiles above the threshold, except within one tile of the
connection's own ends. No corridors are used.

**Capacity from cuts, not the boundary.** v1 sampled only the shared boundary of two tiles. On through-hole boards
that line often falls between pin rows, so the plan saw almost no congestion (Aleste: 1.7 % of tiles at or above
6/8). Capacity is now the narrowest of eight cut lines between the two tile centres, and a free run of *n* samples
holds 1 + (n − 1) · step / pitch tracks. Set-up rises from about 0.2 s to 1–1.7 s on the large boards. Aleste then
has 4.3 % of tiles at or above 6/8, logicbone 4.7 %, P8000 1.5 %, sbc 1.5 %.

**Result.** 18 hard boards (the 10 of §13 plus LFK78, TCKB, P8000, dorkyboard, m2fc, prog_rig, V2X, DronPi), one
variant, 100 M expansions, total connections routed:

| No global routing | Map, from 6, pen 0.5 | Map, from 4, pen 0.5 | Map, from 4, pen 1.5 | v1 corridors (`--global`), new capacities |
|---|---|---|---|---|
| 13,285 | 13,293 | 13,294 | 13,289 | 12,933 |

The map is within noise of the baseline (per board −7 to +4) and corridors stay worse. With honest capacities these
boards are not congested at tile scale: the failures are local pin access, which a 2–5 mm tile cannot see. The
option stays off (D53). The remaining M6 global-routing items (Steiner topology, via capacity, the GPU pattern and
maze port) refine a plan that has no congestion to resolve on this benchmark, so they are deferred until a board set
shows tile-scale overflow; the next lever for clean pass is pin access in the detailed router.

## 17. The last few connections: failure reasons, group re-route, planning by copper distance (2026-10-06)

**Failure reasons were stale.** The unrouted list is taken from the best state, but each reason was read at the end
of the run, after later restarts had overwritten it. Reasons are now saved with the best state; a ripped connection
says which net ripped it, and the last escalation rung (negotiated, with escapes and neck-down) records its reason.
The split in §16 ("roughly 600 boxed in") was made from the stale reasons and is unreliable in its detail. With
correct reasons, nine nearly clean boards (8 variants, 60 M expansions): on five the last one to five connections
are "ripped up by X and not routed again" or "would rip a connection already ripped too often"; ErgoDone has two
pins sealed by fixed copper; teensy-fx and dorkyboard are boxed in before negotiation.

**Group re-route (escalation rung R4, tried and removed).** For each open connection after a restart's passes: lift
the connections that ripped it and those with copper within 2 mm of its pads (five in all at most), route the group
strictly in every order (up to 48, 300 k expansions per search), restore the lifted copper if no order routes all.
Eight nearly clean boards, 8 variants, 60 M expansions: the final routed count was the same on every board (it
closed up to ten connections in intermediate states, never in a state better than the best). 24 boards, one variant:
identical to the baseline on all. The code was removed. On serial_gw the cause is visible in the geometry: IC1 is a
QFN at 0.5 mm pitch whose centre pad has no net, pins 3–6 are GND, +5V, GND, +5V, and with KiCad's default 0.25 mm
track and 0.2 mm clearance the pins can only leave outwards, so the two connections must cross through vias in a
fan-out with no room for them. Ordering cannot fix that.

**Planning by copper distance (on: `--plan-gap`, `--no-plan-gap` for the old behaviour; D54).** The spanning tree of
each net (§11, `plan`) weighed a pad pair by the distance between pad centres. It now uses the gap between the pads'
copper (bisection on the exact `closer_than` predicate, to 1/4096 of the centre distance; touching pads count 0),
with the centre distance as tie-break, and skips pairs that could not beat the best so far. A pin beside a large pad
of its own net (an exposed pad, a wide power pad, a connector shell) is then joined to that pad rather than to the
next pin of the row. Planning takes under 0.2 s more on the largest boards.

Runs with coupled pair routing (`--diff-pairs` or component-rule pair nets) keep centre distances. With copper
distances the `diff_pair` test (USB hub, 15 M expansions) lost pair D3: it was routed coupled, the changed plan of
the surrounding nets led negotiation to split it, and the clean-up found no coupled path again (84 % → 0 % coupled).
Pair routing stays on the plan it was tuned on until re-coupling is more robust.

| Measurement | Centre distance | Copper distance |
|---|---|---|
| 24 hard and nearly clean boards, one variant, 60 M expansions, connections routed | 11,905 | 11,986 (15 boards better, 4 worse, LeeChee +45, LimeSDR +16) |
| Tier D, 8 variants, 120 s, both run side by side (`ab-nogap-tierD`, `ab-gap-tierD`) | 16,618 routed, 12 of 22 clean | 16,622 routed, 12 of 22 clean |
| Tier B against `reach1` | 5,165 routed, 67.5 % clean | 5,166 routed, 67.5 % clean |
| Tier C against `reach1` | 9,523 routed, 63.3 % clean | 9,522 routed, 63.3 % clean |
| Tier A against `m12` | 2,637 routed, 100 % clean | 2,637 routed, 100 % clean |

No board gained KiCad errors. The gain shows at a fixed budget with one variant and not in the KiCad-judged tiers,
where the eight-variant portfolio at 120 s is neutral, as with the reachability pre-check (§13). A first tier D run
made while tiers B and C were also running read 16,576 routed; the side-by-side run shows that was machine load.
Tier D's clean pass is 12 of 22 with either planning: m2fc's last connection closes or not from run to run.

## 18. What the benchmark asks for: designers' layouts and missing pours (2026-10-06)

**The designers' own layouts under the same judge** (`bench/human_baseline.py`; `raw.kicad_pcb` of the 92 boards in
tiers B–D, KiCad DRC with the fixture's rules, errors on routed copper and unconnected items as in `bench/run.py`):

| | Boards |
|---|---|
| Designer's layout clean | 6 of 92 |
| TraceMaker non-clean boards whose designer's layout is not clean either | 33 of 34 |
| TraceMaker clean boards whose designer's layout has errors on routed copper | 52 of 58 (median 108 errors) |
| Designer used copper zones that the unrouted fixture no longer has | 82 of 92 |

PCBench ships no project files, so KiCad's default rules judge boards designed to finer ones; "the designer fails
too" is true almost everywhere and does not tell winnable boards from the rest. Only 32 of the 938 open connections
on the non-clean boards are on a net the designer poured.

**What if the poured nets were left to a pour** (hidden `--skip-net`, 34 non-clean boards, 8 variants, 60 M
expansions, the designer's zone nets skipped): 4 boards complete (serial_gw, mechkeys_58r, LeeChee, LimeSDR); open
connections 938 → 723; large drops on poncho_fpga (46 → 3), MonApollo (58 → 25), V2X (24 → 9), prog_rig (11 → 3);
no change on EtherCAT, P8000, memsarray, robomezzi. Ground and power routed as tracks take channel space on some
boards, but most of the open connections remain: the fan-outs do not fit under the default rules. Keeping or
creating pours for power nets is a possible feature (not built); it would close a few boards, not most.

## 19. Global router v2 and the close of M6 (2026-10-06)

**Built** (`route/global_router.cpp`). On top of §16's cut capacities: connections of one net share tile edges (an
edge carries a net once, and a connection pays no congestion on an edge its net already holds, so a net's
connections merge into a tree with shared trunks: the demand side of a Steiner topology; the tree's pad pairs still
come from the detailed router's spanning tree); a via capacity per tile (free sites of the smallest class via on a
grid of via pitches, at most 8 × 8 a tile) with its own overflow and history; corridors on the layers a path uses
instead of whole via columns; integer costs (256 per tile step), ties broken by node index. Test `global_route`:
`--global` and `--global-congestion` are repeatable byte for byte.

**Result.** 18 hard boards, one variant, 100 M expansions, connections routed:

| No global routing | v2 corridors (`--global`) | + confinement | v2 congestion map |
|---|---|---|---|
| 13,305 | 12,967 | 12,735 | 13,324 |

Corridors cost completion on 15 of 18 boards (prog_rig −99, Teensy −53, Aleste −39), as in §13 and §16. The
congestion map is within noise (decelerator +15, P8000 +5, the rest equal). All global options stay off (D56).

**Why there is no GPU global router.** Routing and negotiating the tile graph takes 0.02–0.5 s per board (Aleste
0.06 s, logicbone 0.52 s, sbc 0.13 s); set-up, which samples the CPU obstacle model, takes 0.4–1.3 s. Against a
120 s routing budget there is at most half a second for a GPU port of the pattern and maze stages (GAMER, GGR) to
save, on a plan that does not help. Not built. Known wart: set-up takes 135 s on LeeChee, where obstacle queries are
about 50 times slower than elsewhere (cause not found; only the opt-in global options pay it).

**GPU DRC broad-phase: not built, the DRC fixed on the CPU instead** (D55). Profiling vme-wren (38,062 copper
items, 128 zones): the uniform grid builds in 14 ms and yields 327 k candidate pairs; of the DRC's 730 s, 395 s
tested items against every edge of each zone fill (`closer_than`, `gap`), 166 s did the same for holes, 80 s
re-evaluated `intersectsArea` for the same fills, 5 s tested fills against the board edge. `geom::PolygonIndex` now
answers any shape against a fill from the nearby edges (`shape_closer`, `shape_gap`), and the rule engine caches
area functions per fill. vme-wren 730 s → 7 s, jetson 250 s → 4 s (connectivity is now the largest part, 2.6–4.4 s);
the test suite 694 s → 334 s. The linear tests stay as the reference path (`drc --linear-zones`): reports are
byte-identical on vme-wren, jetson and six smaller zone boards (tests `drc_zone_index` and the `[geom]` equivalence
test). A broad-phase on the GPU would speed up 14 ms.

**M6 gate.** CPU and GPU give identical results for the one GPU workload routing uses (cost-to-go fields, doc 07);
time to route on dense boards came down through the fields and the reachability pre-check (§13) with no loss of
quality. Global routing did not contribute and is kept as measured, optional code.

## 20. Escalation rungs R4–R6 (M8, 2026-10-06)

**R6, cut proofs: built** (`route --cut-report`; `CutLine` in `route/global_router.hpp`). Every straight line across
the board at the capacity samples (eight per tile boundary, both axes) gets a capacity, the tracks that fit across
it on all layers past the fixed copper, and a demand, the nets that must cross it. A net counts when one of its
planned connections has its two end pads wholly on opposite sides of the line and no pad of the net lies across the
line (such a pad carries the net over without a track). Capacity is an upper bound (a narrow probe, the smallest
class pitch, free runs counted generously), demand a lower bound, so demand > capacity proves that the placement
cannot be routed whatever the router does; the line and the deficit are the request to the placer (doc 04 §4). The
first version counted nets by pad centres and "proved" MicroMaple unroutable across a line through its edge
connector's pads; the pad-box rule above removed that.

On the 92 boards of tiers B–D: no line is over-full. The tightest line carries 9 % of its capacity at the median,
12 % on the non-clean boards, 40 % at most. With §16 (tile congestion) and §19 (corridors) this is the third
measurement that says these boards are not short of space at board scale: what fails is the fan-out at a package.
The report is therefore off by default and costs the global router's set-up when asked for.

**R4, exact window solve: not built.** The design called for a multi-commodity-flow model in CP-SAT, which is not
available here (no OR-tools C++ library). Its cheaper stand-in, the group re-route of §17 (up to five connections,
every order), changed no board's result, and the cut and congestion measurements above say an exact window model
would mostly return "feasible, the heuristic just did not find it" on fan-outs it cannot represent at 2× pitch.

**R5, trial ordering: not built.** Ordering the last connections by how many others each traps is what the group
re-route explored exhaustively on small groups, without effect (§17).

## 21. Micro vias (M12, 2026-10-06)

`--micro-vias` (off): on boards whose rules allow micro vias and with more than two copper layers, a layer change
between an outer layer and the layer next to it may use a micro via of the net class's size (`uvia_diameter`,
`uvia_drill`, not below the board minimums) where a through via is blocked. The search tries it before a
blind/buried via; the commit keeps a through via wherever one is legal, so micro vias appear only where they are
needed; the exact check uses the micro via's own size over its two layers. No fixture allows micro vias on more than
two layers; on two four-layer boards with the setting switched on (EUC-VESC BJT, stm32f407riser; 20 M expansions)
routing was complete or unchanged, KiCad reported no error on routed copper, and no micro via was needed. Like
blind vias (§11): correct, no measurable gain on the boards at hand.

## 22. Successive halving for the portfolio (M10, 2026-10-06)

`--halving` (off): with more variants than threads, every variant runs on a small budget, the better half runs
again on twice as much, until as many are left as there are threads (Jamieson and Talwalkar, AISTATS 2016). Each
rung gets an equal share of the thread-time that `threads` variants would have used, so total work is the same. A
router run cannot be resumed, so every rung starts its variants from scratch; the best result of any rung is
returned, and the selection is deterministic with a work budget.

24 hard and nearly clean boards, 30 M expansions per thread, connections routed:

| Threads | The first variants, whole budget | All eight, successive halving |
|---|---|---|
| 2 | 12,402 | 12,385 |
| 4 | 12,508 | 12,451 |

Halving loses a little: its finalists run on a third (two threads) or half (four) of the budget, and choosing among
eight variants does not make up for that. With eight or more threads there is nothing to allocate. It would need
resumable runs to pay; kept off (D59). The knowledge base's variant choice (doc 06 T3) remains the way fewer
threads pick their variants.

## 23. Escape planning: what is left of M9 (2026-10-06)

Two items of §12 and §14 stay unbuilt, and M9's gate is not met.

- **Negotiated-congestion fallback for escapes.** The flow planner it would fall back from (§14) routes fewer
  connections than the simple corridors of §12 (7 deep-array boards at a fixed budget: no plan 2,687, corridors
  2,736, flow 2,703), so it is off, and the corridors are reservations that the router's ordinary negotiation
  already overrides at twice the crossing cost. There is nothing for a second negotiation to arbitrate.
- **Escape templates in the knowledge base.** A template would store a package's corridor plan for reuse. Planning
  takes 0.1–2 s per board (§12) and its benefit is a few connections on some boards, so a cache of it would save
  no measurable time and add no routed connection.

**Gate** ("the BGA boards of the academic set complete"): not met. On the 17 PCBench boards with BGAs and dense
packages, 41 % are clean, 47 % of the 15 on which every pin can escape under the rules the judge applies (§12);
Freerouting 2.5 completes 24 %. The limit is the same one §16–§20 measure: fan-outs under KiCad's default rules, on
boards whose own routed versions do not pass those rules.

## 24. Differential pairs: twists, per-pair skew, and what the failures are (M12, 2026-10-06)

**Why pairs stay uncoupled** (`TM_DEBUG_PAIRS` now tags each attempt; 18 boards, 59 pairs, first attempts): 63 of
120 attempts couple. Of the failures, 26 are tagged "pin order mirrored between the ends" and 8 "an end pad already
carries routed copper"; but 20 of the 26 are one pair (OtterPillG's USB pair, from a bottom-side connector with no
via site) tried again and again, and pairs that join routed copper mostly succeed (13 couple, 8 do not). 24 of the
59 pairs have a half with more than two pads. So neither situation is the large class the earlier note assumed.

**Twists** (on with pair routing; `--no-pair-twists`). A new move of the coupled search (`kTwist`): the right half
changes layer first, the halves cross while they are on different layers, the left half changes layer last, and
the search continues on the new layer with the sides swapped. In the pair's frame, with D = offset + via offset:
right half (0, −off) → via (hj, −vo) → (hj + D/2, −vo) → (hj + 3D/2, +off); left half (0, +off) → (hj + D/2, +off) →
(hj + 3D/2, −vo) → via (hj + 2D, −vo) → (2hj + 2D, −off). Each track keeps at least D from the other half's via,
which is the distance the coupled via pair keeps; both halves get one via and the same length. Every segment and
via is checked exactly, and the finished pair is verified like any other. It costs a via pair plus its uncoupled
length, so the search uses it only where the pin order asks for it.

18 boards, 59 pairs, 30 M expansions: one pair gains coupling (USB-C-Screen-Adapter L3: 0 → 86 %), none loses;
pairs at ≥ 80 % 18 → 19, at ≥ 50 % 35 → 36, median share 57 → 63 %; connections routed 2,210 → 2,212. The
`diff_pair` test is unchanged (five pairs 91–94 % coupled, KiCad-clean).

**Per-pair skew limits.** `RouterOptions::pair_net_skew` gives each pair of `pair_nets` its own limit; pairs found by
the component rules take USB2-02's `max_intra_skew_mm` (1.27 mm). A custom skew rule of the board wins, then the
pair's own limit, then `--pair-skew-mm`.

**Still not built:** a pair ending on routed copper as such (the pair's ends are pads; the count above says it
rarely matters), pair-aware global routing, arcs, skew in picoseconds, both halves meandering together.

## 25. Finer lattice instead of a gridless arm; learned models (M12, 2026-10-06)

**The question behind a gridless router** is whether geometry finer than the lattice finds routes the lattice
misses. Twelve nearly clean boards, all eight variants, default pitch at 30 M expansions against half the pitch at
120 M (the same coverage per unit of work):

| | Default pitch | Half pitch |
|---|---|---|
| Connections routed (12 boards) | 4,140 | 4,153 |
| Boards complete | 0 | 1 (robomezzi 282 of 282) |
| Better / worse boards | | 9 better (teensy-fx +9, V2X +5, MicroMaple +4), 2 worse (dorkyboard −8, mechkeys_58r −4) |

So finer geometry helps some fan-outs and costs four times the work. It is taken as a portfolio arm: the
cheap-vias variant routes at half pitch when the lattice has fewer than a million points per layer (it already
routes large boards at twice the pitch). Side by side on the KiCad-judged tiers (8 variants, 120 s, with and
without the arm): tier B 27 → 28 clean of 40 (USBI2C01 and SALSAFLOCK gained, serial_gw lost) and 5,156 → 5,163
routed; tier C 18 → 18 clean of 30 and 9,506 → 9,514 routed; no board gained errors. On (D63; `--no-fine-variant`).

**A gridless tile-plane arm is not built.** The half-pitch result bounds what it could add on these boards (about
0.3 % more connections), and the failures that remain are fan-outs that do not fit under the default rules at any
resolution (§17, §20). It would be a second router; the finer variant takes most of the gain for a line of code.

**Learned ordering and congestion models are not built.** What they would learn is measured to matter little here:
every order of small conflict groups changed no board (§17), jittered and reversed orders are already portfolio
arms whose wins are spread evenly (each of the eight variants wins between 9 and 46 of 172 boards), tile congestion
is absent (§16, §20), and the knowledge base's bandit already chooses variants per board from past runs (doc 06).

## 26. Plane-aware routing of all-SMD boards (2026-10-07)

**The case.** Small boards put every part on the outer layers and give whole inner layers to ground and supply
planes: no pad touches a plane, every plane pin needs a via, and the planes cover the board. On one such board (a
private 6-layer wearable: two GND planes and a supply plane inside, a 0.4 mm-pitch WLP, a 0.5 mm-pitch QFN, a BLE
module, a 32 kHz crystal; 167 connections) the router stopped at 94/167 with no via at all:

1. Zone fills are fixed copper of their net, so a via of another net through a full-board plane was illegal
   everywhere, and a GND via crossed the supply plane. Rule areas "no tracks, vias allowed" on the plane layers,
   which keep the planes whole, blocked vias too (D65).
2. `plan` drops zone-only clusters, so a plane that no pad touches was never a target and GND pads were wired to
   each other on the signal layers.
3. With planes as targets, the cheapest plane connection is a via at the pad centre: about 100 vias landed in pads
   (0.5 mm vias in 0.25 mm QFN pins and 0402 pads).
4. An inner WLP ball (0.25 mm lands, 0.15 mm gaps) looks free locally (the diagonal gap fits a track), but every exit
   ends on another ball, and there is no dog-bone site at 0.4 mm pitch.
5. The 32 kHz crystal line, routed in shortest-first order and ripped by negotiation, ended 25.7 mm long with four
   vias.

**What was built** (all opt-in except the keep-out fix):

| Part | What |
|---|---|
| Keep-outs (D65) | A rule area blocks tracks if it forbids tracks and vias if it forbids vias. Before, only areas forbidding tracks were read, and they blocked vias too |
| `--soft-zones` (D66) | `Obstacles` skips zone fills in `copper_state` and `fixed_code`; KiCad refills around the new copper. `plan` keeps zone-only clusters of a net that has pads, when their fill is at least 1 mm² (a sliver of a stale fill is no target) and a pad of the net can reach it, and never plans a connection between two zone-only clusters. Teardrops are never targets, and a via joins every same-net plane it passes through (§32) |
| `--keep-vias-off-pads`, `--vias-off-pads-below MM` (2; D67) | A via probe treats single-layer pads narrower than the limit, of any net, as obstacles at net-class clearance, also in the cached `fixed_code` (net-independent, as the cache needs). Exposed pads still take vias |
| `--via-in-pad` (D68) | `pad_cells` offers the pad centre on every other layer, with the board's minimum via (`min_via_diameter`, `min_through_hole_diameter`; no via when they could not be read, with a warning), for inner balls (a round pad with a pad of its footprint one ball pitch away in each direction of the footprint's frame, found once per board) and for pads with no legal own-layer cell. The penalty is four vias at the current via price, charged on either end (`src.cost`, and on entry to a target cell). One via per pad: a pad whose via another connection placed is not offered again. `commit` adds the via, checked with the pad exemption |
| `--first-nets A,B` (D69) | Connections of the named nets go first in `plan`'s order (after learned priorities) and again after every restart; `commit` and the via clean-up (`lns_vias`) do not rip them for other nets |

**Measured.** Synthetic board `tests/boards/plane_smd` (4 layers, every part SMD, filled GND / +3V3 planes inside,
3 M expansions, 2 variants; KiCad 10 DRC after `--refill-zones`):

| Options | Routed | Vias | GND / +3V3 track | KiCad errors / unconnected |
|---|--:|--:|---|---|
| defaults | 41/47 | 0 | 39.7 / 86.6 mm | 0 / 6 |
| `--soft-zones` | 48/49 | 38 | 6.0 / 1.6 mm (vias in pads) | 0 / 1 |
| + `--via-in-pad` | 47/49 | 40 | 6.0 / 1.6 mm | 0 / 2 |
| + `--keep-vias-off-pads` | 49/49 | 42 | 15.9 / 10.7 mm | 0 / 0 |
| + `--first-nets XIN,XOUT` | 49/49 | 42 | 15.9 / 10.7 mm | 0 / 0 |

`--via-in-pad` belongs with `--keep-vias-off-pads`: alone, plane vias crowd the edge of the ball array and box in two
edge balls. `--first-nets` changes nothing here (the crystal is not contested); on the private board it took the
crystal lines from 25.7 mm with four vias to 4.3 and 5.1 mm with one via each. Default behaviour and the quick tier:
see the pull request that brought this section (numbers measured on the merged tree).

**Not built.** The options are in no portfolio variant and are not chosen automatically (a board with inner planes and
only SMD pads could switch `--soft-zones` and `--keep-vias-off-pads` on by itself; the component-rule catalogue could
feed crystal nets to `--first-nets`). The output keeps the old, now stale fills: refill before judging. Connectivity
still trusts the stale fills during routing (a fill that a refill would split is one target); since §36 the route
job refills in memory afterwards and routes what the refill left unconnected. `--first-nets` takes net names only,
and the protected nets may rip each other. A second connection at a pad that already has a via in it must use the
pad's own layer.

## 27. Custom-rule routing (2026-10-06, D72)

**Before.** Any custom rule disabled the obstacle cache and cost-to-go fields.

**What was built**

| Rule | Router | DRC (`tracemaker drc`) |
|---|---|---|
| `disallow track` by net, net class, type or layer, including `inDiffPair` | `RuleEngine::track_allowed` supplies per-net layer masks for pad cells, escapes, planar moves, via landings, diff-pair legs, escape corridors and fields. Through vias may pass through disallowed track layers. | `items_not_allowed`, once per item |
| `disallow via`, `through_via`, `micro_via`, `buried_via` or `blind_via` with those conditions | `via_allowed(net, type)` is a per-net switch for each via type. `via` or `through_via` matching a through via on any layer leaves the net without vias of any type (blind, buried and micro vias are only tried where a through via is blocked). Blind/buried router probes have no span yet, so either keyword conservatively stops both; `micro_via` alone stops micro vias. | `items_not_allowed`; `blind_via` has exactly one outer span endpoint, `buried_via` none, micro remains separate (D80). |
| Positional, footprint or pad-dependent `disallow` (`insideArea`, `intersectsArea`, `enclosedByArea`, courtyard functions, `memberOfFootprint`, `Reference`, `Pad_Type`, `Width`, `Size_X/Y`, `Position_X/Y`) | Applied to every new track and via with its actual shape, width, position and span, in the exact checks before commit, escapes, pair legs and clean-up, and in the search's lattice tests (§34, D82). Not warned. | Nanometre-valued properties and comparisons; intersection/enclosure of copper, common area layers and physical front/back courtyard polygons (§33, D80). |
| Rules whose condition does not parse, uses an unknown property/function anywhere, or contains a single numeric literal without units | Not applied; warned by name. | Not reported, including an unknown symbol in a short-circuited branch. `Parent.Reference` is not an alias (D80). |
| Rules that use a KiCad function TraceMaker does not evaluate (`hasNetclass`, `hasExactNetclass`, `hasComponentClass`, `fromTo`, `isMicroVia`, `isBlindBuriedVia`, `isBlindVia`, `isBuriedVia`, `isCoupledDiffPair`, `getField`, `memberOf`, `memberOfGroup`, `memberOfSheet`, `memberOfSheetOrChildren`, `insideCourtyard`, `insideFrontCourtyard`, `insideBackCourtyard`) | Kept, because KiCad applies them; warned by name. The call is taken as false, so the rule holds where the rest of its condition decides: `A.NetName == 'X' \|\| A.hasNetclass('Y')` still applies to X. | The same; a `disallow` whose evaluation reached the call is not reported (D80). |
| Matching `disallow` with `(severity ignore)` | Later matching rules for the same item type win, including ignore exceptions. | Ignore clears that item type's selected violation; different disallow types accumulate (D80). |
| Item `A.Layer` versus `(layer ...)` / `existsOnLayer` | Static track/via gates retain the same distinction. | Own track layer; pads use footprint side, including PTH; vias have no layer property. A zone uses its source layer, not a fill projection; a multilayer zone has an unset layer ID (`==` false, `!=` true). Layer selectors and membership still examine occupied copper layers (D80). |
| `disallow hole / footprint / text` | Not applied; warned by name. | Left to KiCad |
| `physical_hole_clearance` | Between a hole and any other item's copper, whatever the nets, as KiCad reports it. `Obstacles::physical_copper_state` tests new tracks and via pads against the holes of pads and vias; `physical_hole_state` tests a new via's hole against copper; both against fixed and routed items, in the exact check before commit and in the search's cached and routed checks. A net whose vias the rule sets against its own tracks (an unconditional rule does) gets no vias: the tracks that end in a via touch its hole. For the same reason a track cannot end on a plated pad of its own net under such a rule. | `hole_clearance`, once per hole and item, any net |
| Keepout rule areas | Tracks and vias use their respective keepout flags (§26, D65). | Unchanged |

Disallow masks retain the per-class cache. Physical-hole rules do too when independent of `NetName`, `NetClass`
and `inDiffPair`. Other custom rules, including unparseable conditions, require exact checks.

KiCad's violation names and counts were checked with kicad-cli 10.0.3. Example: inner layers for GND only;
no via in an SMD pad except on U1.

```
(rule "Inner layers carry GND only" (layer inner) (condition "A.NetName != 'GND'") (constraint disallow track))
(rule "No via in SMD pad (except U1)" (constraint physical_hole_clearance (min 0.05mm))
  (condition "A.Type == 'Via' && B.Type == 'Pad' && B.Pad_Type == 'SMD' && B.Reference != 'U1'"))
```

**Results.** On a private 4-layer sensor board (218 connections), the example rules give parity on all 120
`items_not_allowed` violations and 63 of KiCad's 64 `hole_clearance` pairs. The missing pair is a paste-only pad
on U2, absent from the copper model; the same via is reported against its copper pad. Three existing J2/J5 hole
clearances from two fixed footprints are reported by TraceMaker but not KiCad.

At the same placement, 20M work and 8 variants: without rules, 218/218 routed, 119 non-GND inner-layer tracks,
64 vias in SMD pads and one via in J5's keepout. With rules, 208/218 routed, no inner-layer track, no via in a
non-U1 SMD pad, nothing in J5's keepout and no added KiCad DRC error.

## 28. Search speed on fine lattices (2026-10-06, D73)

**Before.** On the private board, project rules give a 0.05 mm lattice. The placement loop's 20M-work route
check (8 variants) took ~140 s, including ~1–2 s for Metal fields. CPU profiling (`sample`) identified these costs.

**What was built**

| Part | Cost before | Change |
|---|---|---|
| `DesignRules::class_for`, `Router::Impl::cache_for` | ~80% in wildcard/regex lookup | Resolve classes once per net; retain the last cache lookup. |
| `Probe` (`obstacles.cpp`) | ~20% after class caching | Reuse rule-probe items in a per-thread pool. |
| Via checks, `Obstacles::routed_via_state` | ~35% of search | Skip checks when the bare via cost cannot improve another layer; check layer-independent routed holes once. |
| PathFinder history | ~9% of search | Cache history with cell state; history changes only between searches. |

**Results.** The 20M check fell from ~140 s to ~10 s; the 5M check from ~32 s to ~4 s. Outputs at both budgets
are byte-identical. Fewer filled via-cache entries leave more cells unknown to the field, increasing total
expansions by 0.01% without changing output there (see "Checked at the merge" below: identity is measured, not
guaranteed).

**Second round** (2026-10-06). On the same 20M check, `/usr/bin/time -l` instructions retired repeated within
0.2%; wall and user time varied ±20% with shared machine load. Runs used `--threads 2`; work-budget output is
thread-count independent. Peak memory is ~0.4–0.8 GB per concurrent variant (2.8 GB at 2 threads, 6.3 GB at 8).

| Part | Change | Effect |
|---|---|---|
| `Obstacles::fixed_via_code` | One copper query across layers; check holes, edges, keepouts and mask openings once. Grid tests compare with `fixed_via_code_reference`. | user CPU −7% |
| `Shape::set_point`, `closer_than_disk` | Reuse scratch disks and test routed via holes without shape allocation. | user CPU −10% |
| `seg_seg_closer` | Reduce a degenerate segment to one point-to-segment test; exact parity with the general four-way test. | instructions −3.5% |

Outputs remain byte-identical at 5M and 20M on the private board; quick tier 30/30.

**Public boards** (`bench/speed_ab.py`, Apple M4 Pro): the engine before and after D72–D73 on the same boards,
one variant, one thread, CPU fields, fixed work. Instructions retired from `/usr/bin/time -l`.

| Set | Work | CPU s before → after | Instructions | Speed-up | Output |
|---|--:|--:|--:|--:|---|
| 10 large PCBench boards (`bench/m6_bench.py` set; no project files, default rules) | 30M | 100.7 → 83.2 | 1,511 G → 1,221 G | 1.21× (1.09–1.32×) | 10/10 identical |
| 6 small KiCad demo projects (`--demos`; net classes with name patterns) | 2M | 184.1 → 75.8 | 5,023 G → 2,019 G | 2.43× (2.15–2.73×) | 6/6 identical |

The gain is largest where net classes use name patterns, which the old per-cell lookup matched every time.
The two largest demos (vme-wren, jetson-agx-thor) route nothing within 1M work and take 128 s and 60 s doing so
(jetson 2.1× faster after, vme-wren unchanged); where that time goes is not yet profiled.

**Checked at the merge** (2026-10-07, Linux, GCC 15, x86-64; the merged tree against the router before it, both
with CPU fields, `--seed 7`, no knowledge base):

| Set | Budget | CPU s before → after | Speed-up | Output |
|---|--:|--:|--:|---|
| 8 PCBench boards (1Bitsy, Aleste-520EX, chirp, CoreOne, MonApollo, decelerator4030, logicbone, sbc) | 3M, 8 variants, 1 thread | 49.9–78.9 → 38.4–63.5 | 1.13–1.49× | 8/8 identical |
| KiCad demos complex_hierarchy, pic_programmer (project rules, net classes with name patterns) | 3M, 8 variants, 1 thread | 1,079 → 475; 1,364 → 613 | 2.27×, 2.22× | 2/2 identical |
| KiCad demos RoyalBlue54L-Feather, CM5_MINIMA_3 | 1M, 2 variants | 173 → 74; 773 → 338 | 2.33×, 2.29× | 2/2 identical |
| 36 further multilayer PCBench boards | 600k, 3 variants | not timed | | 36/36 identical |

The machine was shared, so the times are indicative. Output is also the same at 1 and 3 threads (2 boards).

*Identical output is measured, not guaranteed.* The via-check skip leaves fewer via codes in the per-class cache,
the cost-to-go field then knows fewer blocked via cells, and the searches expand a different number of states:
on 1Bitsy five of the seven variant lines compared differ (for example 941,956 → 945,052 expansions) while the
routed board is byte-identical. The work budget counts expansions, so where a budget ends mid-search the two
routers can stop at different points. All 48 boards above gave identical files; a board that does not is
possible, and the skip has no switch to turn it off for a comparison.

*Reference paths (CLAUDE.md rule 3).* Tested against a reference: the one-pass `fixed_via_code` against
`fixed_via_code_reference` on a grid of points (with custom rules, and with soft zones and vias off pads, §26),
and the degenerate-segment path of `seg_seg_closer` against the four-way test on 60,000 random cases. Checked by
reading only, with no test of their own: `closer_than_disk` against `closer_than` with a point shape,
`routed_via_state` against the per-layer `routed_state` loop it replaced, the history cost cached with the cell
state, the per-net class table, the last-lookup memo in `cache_for`, and the via-check skip itself. The
byte-identical boards exercise all of these together.

**Reverted.** A 4-ary open heap and pre-rule bounding-box filters gave no measurable gain. Combining routed
queries was slower; per-layer/via-only routed indexes gave −2% within noise and added five indexes; a combined
physical-hole query gave −0.3%.

**Remaining costs.** Open list ~16%, routed-copper queries ~17%, fixed-copper via checks ~16% (half rule
evaluation), fields ~10%. Fixed-obstacle caches remain per variant: up to eight copies of the same lattice
codes; sharing them needs a thread-safe cache.

## 29. Layer limits: layers without tracks, and dearer layers (issue #6, 2026-10-07)

**The problem.** On a signal / power / ground / signal stack the router treated all four layers alike and put
about a third of its track length on the two plane layers. There was no way to say "keep the planes whole" or
"use them only when nothing else works".

**Two options, both off by default** (layer names as KiCad writes them, the file's own names, or the user's
layer names; an unknown layer or a bad factor stops the run with an error instead of being dropped):

- `--no-tracks-on In1.Cu,In2.Cu`: these layers get no new tracks. Vias still pass through them, and a via may
  still end a connection in the net's own zone fill there, which is how a pad reaches its plane. A pad that
  exists only on such a layer has no legal exit and stays unrouted (the CLI counts them in a warning).
- `--layer-cost In1.Cu=4,In2.Cu=4`: a track on that layer costs that many times its length (1 to 1000). The
  layer stays available; the search takes it only where the detour on the plain layers would cost more.

**How.** A layer without tracks is taken out of every net's allowed-layer mask, the same mask that custom
`disallow track` rules fill (§27), so the A*, the reachability pre-check (§13), the cost-to-go field, pad
endpoints and escapes, the coupled-pair search and the escape planners all see one thing. Two additions serve
both: a via may land on a layer the net has no tracks on where that cell ends the connection in the net's own
zone fill, and `commit()` rejects any segment on such a layer before the exact check, whatever produced the
path. The cost factor multiplies the planar step cost per layer in integer per-mille arithmetic (rule 2).
Factors below 1 are refused: the octile and field heuristics are lower bounds only if no track costs less than
its length. With both options unset, no cost or test changes, and the output is byte-identical (10 PCBench
boards at 5 M work units; the quick tier is unchanged).

**Measured** on six four-layer PCBench boards (4 variants, 60 s per board, KiCad as the judge):

| | Routed (of 1,078) | Boards complete | Track on In1/In2 | Added DRC errors |
|---|--:|--:|--:|--:|
| Defaults | 1,058 | 5 | 2,193 mm of 6,512 (34 %) | 0 |
| `--no-tracks-on In1.Cu,In2.Cu` | 1,038 | 3 | 0 mm of 6,467 | 0 |
| `--layer-cost In1.Cu=4,In2.Cu=4` | 1,054 | 4 | 430 mm of 6,734 (6 %) | 0 |

**Limits.**
- The limits apply to every net. A per-net limit is a KiCad custom `disallow track` rule (§27).
- A dear layer that cannot be avoided makes that search wider: the heuristics measure plain length, so the
  search first exhausts everything cheaper (a 15-fold expansion count on the test board where every way across
  costs four times). A field computed with per-layer step costs would remove this; it needs the same change in
  the CPU reference and the CUDA kernel and is not built.
- The global router and the cut report (§16, §20) still count capacity on every layer, so `--cut-report` may
  call a board routable that is not once layers are closed. Its proofs of "unroutable" stay valid.
- Planes that no pad touches are still not connection targets; only a net whose fill already holds one of
  its pads gets vias into it.
- The Python bindings and the KiCad plugin do not expose the two options yet.

## 30. Coupled-pair search: rejected candidates are work (2026-10-08)

**Before.** A pair attempt (§15) counted only the states it closed. Building and checking the legs of start and goal
candidates was free, and the attempt never looked at the work budget or the time limit until it ended. On a board
whose fills hug its pads every leg fails, so an attempt checks tens of thousands of candidates at about zero work:
stripped CM5 with its fills kept and `--diff-pairs` ran 1,048 s at `--time 900` with 130,247 work units counted, and
at `--work 20000` it ran 635 s. A sample put 98 % of the time in the start legs' exact checks against fill polygons
(17,742 vertices for the largest).

**What was built** (D77). Every exact segment check of a leg, legal or not, is a work unit, as is every closed
state. The attempt adds its work to the router's total every 64 steps (a step is one candidate or one state) and
then checks the budget and the time limit, so a budgeted run stops inside an attempt, deterministically, and a
long attempt stops at the time limit instead of finishing first. Nothing is committed before the attempt's goal
check, so stopping leaves no partial pair. The attempt's state budget is unchanged and leg checks get no cap of
their own: a version that capped them at the state budget lost coupling on three of 18 boards at 30 M, where a
pair needs many rejected candidates before its first legal one. `RouteResult::pair_work` and the route summary's
`pair_work` report the part of the work spent in pair searches.

**Results.**

| Check | Before | After |
|---|---|---|
| CM5 (fills kept), `--diff-pairs --time 4` | 15.8 s | 4.1 s |
| CM5 (fills kept), `--diff-pairs --work 20000` | 635 s, 34,754 counted | 3.4 s, 20,051 counted, same output at 1 and 2 threads |
| Quick tier (no pairs), 1 M work | – | byte-identical on all 30 boards |
| `diff_pair` test (USB hub, 15 M) | 113 / 113 | byte-identical, 429,115 of the work in pairs |
| 18 pair boards of §15, `--diff-pairs`, 30 M | – | 17 byte-identical; PmodHDMIIn routes 112 / 116 as before with less coupling (54 8 41 56 → 44 8 4 48 %), because 958,420 of its 30 M now go to leg checks; at 30,958,420 the output is byte-identical to the old 30 M |

Results at a fixed `--work` with pairs on therefore shift a little: the same budget buys less routing where pair
searches check many candidates. The time limit now ends a pathological attempt, but a work budget alone still does
not bound its wall time: one leg check against a fill of 17,742 vertices costs about 0.3 ms, because
`Obstacles` tests a shape against a whole fill polygon. Answering it from nearby edges with `geom::PolygonIndex`,
as the DRC does (§19), is the speed fix and is not built here.
## 31. Router obstacle tests against zone fills through the edge index (2026-10-08)

**Before.** The DRC answers shapes against zone fills from nearby edges (§19, D55), but the router's `Obstacles`
still tested every probe against every edge of every fill it came near: the fixed-copper codes of the lattice,
the exact segment and via checks, and physical hole rules. On boards whose fills have thousands of vertices this
was most of the run: stripped CM5 with its fills kept spent 21 s on 2 M work units, and one coupled-pair leg check
against its largest fill (17,742 vertices) cost about 0.3 ms.

**What was built** (D78). `Obstacles` builds a `geom::PolygonIndex` for each zone fill at construction (the DRC's
`drc::ZoneFills`) and asks it in place of the per-edge loop wherever a probe meets a fill (`item_closer`). The
index's answer equals `closer_than` against the fill (the `[geom]` equivalence test); the linear loop stays as the
reference path (`Obstacles::set_linear_zone_tests`), and a test compares the two over segment, disk, via,
fixed-code and fixed-via-code queries around a fill with 160 teeth and a notch, with a physical hole rule.

**Results.** Same work budget, one variant, CPU fields; every output byte-identical to the old binary.

| Boards | Before | After |
|---|--:|--:|
| `speed_ab.py --demos`, 10 M (six KiCad demos with fills and project rules) | 514.9 s, 7,329 G instructions | 118.7 s, 1,066 G (tinytapeout 200 → 7 s, kit-dev-coldfire 56 → 5 s, CM5_MINIMA_3 175 → 93 s) |
| vme-wren / jetson-agx-thor, routing stripped, 1 M | 29.5 / 49.3 s | 5.8 / 3.8 s |
| CM5 (fills kept), 2 M, two threads | 21.0 s | 1.6 s |
| CM5 (fills kept), `--diff-pairs`, 2 M, with §30 | 351 s | 2.3 s |
| `speed_ab.py` PCBench, 10 M (10 boards) | 52.6 s | 52.4 s |
| Quick tier, 1 M (30 boards) | – | byte-identical |

Peak memory is unchanged (tinytapeout at 10 M: 2.82 GB peak footprint both); the index adds about 3 MB there.
`speed_ab.py` reports resident size, which macOS lowers by compressing pages in long runs, so its "before" column
reads lower on the slow boards.

**Limits.** Fills are indexed as read; the router never changes them, so the index stays valid (`--soft-zones` skips
fills before they are tested). The plane-target test of a soft-zone connection (inside the fill and no edge within the
track's half-width) still walks all edges, once per lattice cell it asks about; its answers are cached per cell.
## 32. Plane targets: teardrops, unreachable planes, and planes a via passes through (2026-10-08)

**Before.** Three ways the plan of §26 asked for connections that could not or need not be made:

1. A teardrop (a zone with `(attr (teardrop ...))`) was a zone like any other. When routing is deleted, the
   teardrops stay; an orphaned one of a signal net became a plane target with `--soft-zones` (RoyalBlue54L: zone
   targets on SWDCLK, RESET and SDA), and in every mode a teardrop attached to a pad was a target for the net's
   other pads.
2. A plane that no pad of the net could reach (no vias for the net, from `disallow via` or without vias at all)
   still replaced the pad-to-pad connection: each pad got a connection into the plane, and none could be routed.
3. A through via from a pad into one plane passes through the net's other planes, but the plan's connection to
   the second plane was routed anyway, with another via.

**What was built** (D79).

- The reader keeps the teardrop attribute (`model::Zone::teardrop`); `plan` never makes a teardrop a target.
  Teardrops stay copper of their net for connectivity and, with `--soft-zones`, are as soft as other fills.
- `reaches(pad, zone)`: a connection from the pad can end in the fill by a via of a type the net may place (a via
  may end in the net's fill on any layer, §29), or by a track on a layer the pad and fill share and the net may use.
  A zone-only cluster is a target only if some pad of the net reaches it, and a pad is offered a fill only if it
  reaches it.
- With `--soft-zones`, every routed track or via records the same-net plane fills it touches (`drc::zones_touching`,
  1 nm, as the initial connectivity), and `joined` counts those as joined, so a later connection to a plane
  already reached is satisfied. The record follows the item; a rip removes it with the connection.

**Results.** Eight boards, routing stripped, 3 M work units, one variant, two threads; KiCad 10.0.3 after
`--refill-zones`:

| Board | Routed (`--soft-zones`) | Unconnected after refill | Defaults |
|---|---|---|---|
| complex_hierarchy (165 teardrops) | 65 → 84 / 84 | 34 → 3 | 77 → 84 / 84, unconnected 21 → 3 |
| RoyalBlue54L-Feather (263 teardrops) | 67 → 76 / 171 | 107 → 100 | byte-identical |
| multichannel_mixer, StickHub, sonde xilinx, pic_programmer, interf_u, plane_smd | equal | equal | byte-identical |

No board gained a DRC error. The quick tier is byte-identical (no teardrops; nothing changes without them unless
vias are forbidden). Tests (`[planes]`): a teardrop is read as one and is no target; an unreachable plane (no vias,
or a rule forbidding the net's vias) leaves one routed pad-to-pad connection where there were two failed plane
connections; with planes of one net on In1 and In2, two vias instead of three.

**Tried and dropped.** Keeping teardrops hard under `--soft-zones` (as track copper), on top of the target fix and
before the contact credit: RoyalBlue 108 → 100 unconnected at 3 M but 89 both at 10 M, complex_hierarchy equal; no
consistent effect, so they stay as soft as other fills.
## 33. Custom-rule conditions: KiCad 10 parity (2026-10-08)

**Before.** The 61-case custom-rule corpus matched 35 cases and disagreed on 26. Comparisons discarded unit
suffixes, unknown symbols could hide behind short-circuiting despite a "rule not applied" warning, and area
enclosure used bounding-box corners. Vias acquired the DRC pass's layer; ignore rules and blind/buried spans
were not distinguished.

**What was built** (D80).

| Semantics | Deciding KiCad 10.0.3 cases |
|---|---|
| `<`, `<=`, `>`, `>=` and numeric equality/inequality; dimensional properties are nanometres | `width_gt_mm`, `position_anchor_x_le_zero`, `size_x_mm`, `size_y_half_mm`, `size_x_mil` |
| Literal scaling without rounding, retaining fractional nanometres; equality is exact in double precision, so `A.Width == 1.001mm` (1000999.9999999999 nm) does not match a 1.001 mm track. A literal is a sign, digits and an optional fraction: no leading dot, no exponent | `width_eq_mil`, `literal_mm_mil`, `fractional_nm_constant`, `fractional_nm_not_zero`, `number_inexact_*`, `number_leading_dot`, `number_exponent` |
| Exact case-sensitive unit vocabulary: `mm`, `mil`, `in`, `deg`, `fs`, `ps`, including spaced suffixes; time scales to attoseconds | `width_gt_in`, `width_eq_spaced_mm`, `width_eq_deg`, `width_eq_250fs`, `width_eq_quarter_ps`, `literal_ps_fs` |
| One numeric literal without units drops the entire condition, even in an unused branch; multiple numeric literals permit bare internal-unit values | `width_ne_double_or`, `width_gt_double_or`, `width_eq_integer_multiliteral_false_or` |
| Quoted dimensions remain strings: equality with a number is false, inequality true, relational conversion is zero | `size_x_quoted`, `width_eq_quoted_mm_or`, `width_ne_quoted_mm`, `width_gt_quoted_mm` |
| `\|\|` binds tighter than `&&`, and `!` tighter than the comparisons, unlike C: `a && b \|\| c` is `a && (b \|\| c)`; `!A.NetName == 'X'` compares the negation with `'X'` and is never true | `precedence_and_or`, `precedence_or_and`, `precedence_mixed`, `precedence_not_eq`, `precedence_not_ne` (KiCad 10.0.6) |
| Properties and functions unknown to KiCad drop the rule anywhere in the condition (a function KiCad knows and TraceMaker does not evaluate keeps it, see §27); invalid `Parent.Reference` aliases are removed; bare `L` remains undefined for unary disallow | `unknown_short_circuit`, `unknown_function`, `reference_A_Parent_Reference`, `reference_Parent_Reference`, `bare_layer_front`, `kicad_function_*` |
| A terminal unterminated single-quoted string extends to the end | `malformed_quote`; unmatched parentheses still fail (`malformed_paren`) |
| Later matching disallow rules win for the same item type; ignore clears that violation, not another type's ban | `severity_ignore`, `later_ignore`, `earlier_ignore`, `later_different_disallow`, `later_different_disallow_ignore` |
| Own item layer, also in paired clearance: tracks use their layer, pads the footprint side even for PTH/NPTH; vias have no layer property (`==`/`!=` false); multilayer zones expose an unset ID (`==` false, `!=` true), not a fill's layer | `item_layer_front`, `item_layer_back`, `own_layer_pads_front`, `own_layer_pads_not_front`, `zone_pair_*` |
| `insideArea` aliases intersection; common copper layers required; enclosure checks all rounded copper against actual contours, including holes and concavity | `area_insideArea`, `area_enclosedByArea`, `area_concave_*`, `area_hole_*` |
| Front and back courtyards are the footprint's own sides (`GetCourtyard(fp->IsFlipped() ? B_Cu : F_Cu)`): a flipped footprint's front courtyard is its B.CrtYd outline; the item's copper side does not matter. Footprint selectors are reference wildcards, or library-id wildcards when they contain `:` (courtyard functions and `memberOfFootprint`); closed lines/arcs/rectangles/circles/polygons, no interior for an open outline | `court_intersectsBackCourtyard`, `court_flipped_*`, `court_lib_id`, `membership_lib_id`, `court_line_*`, `court_arc_*`, `court_unclosed_*`; direct geometry tests cover circles, polygons and wildcards |
| Absolute transformed pad/via anchors; tracks/arcs have undefined positions (`==`/`!=` both false, relational zero-coercion) | `position_x`, `position_anchor_shifted_pad`, `position_anchor_rotated_pad`, `position_anchor_x_ne_zero`, `position_anchor_x_lt_one` |
| Blind via spans touch exactly one outer layer; buried spans none; micro vias remain separate | `subtype_blind_via`, `subtype_buried_via`, `subtype_micro_via` |

The rejection of `um`, `cm`, `mils`, `inch`, `thou` and uppercase suffixes, and non-conversion of quoted
dimensions, are measured KiCad behaviour, not advertised language extensions. The unusual lone-unitless-literal
rule is also in [KiCad's compiler](https://github.com/KiCad/kicad-source-mirror/blob/10.0.3/common/libeval_compiler/libeval_compiler.cpp);
the unit list is in its [PCB evaluator](https://github.com/KiCad/kicad-source-mirror/blob/10.0.3/pcbnew/pcbexpr_evaluator.cpp).
Courtyards are prepared once, and area/courtyard queries use the existing integer geometry without a new
dependency. The new item-dependent conditions remain positional: the router leaves them to DRC and warns by
rule name. Static gates only acquire dropped-rule and typed ignore semantics; blind/buried probes without
a span retain the conservative router gate.

**Results.** Original corpus: 35/61 → 61/61 MATCH (26 → 0 mismatches). The expanded corpus matches all 166
cases, including 70 scalar/geometry probes and 30 paired-clearance probes. Five were added after a routed
board (kitspace_threeboard, ICs on the bottom) showed that KiCad's front and back courtyards are the
footprint's own sides: flipped-footprint courtyards and library-id selectors. Comparison uses violation
item/pair multisets, retaining repeated reports against different copper-layer fills rather than just counts.
`rule_parity` passes in 1.62 s without KiCad; a full KiCad 10.0.3 rejudge reproduces the frozen oracle.

**Re-judged with KiCad 10.0.6 (2026-10-09).** A full rejudge with KiCad 10.0.6 reproduced all 166 cases, and
`expected.json` is now frozen from 10.0.6. Conditions probed beyond the corpus showed differences, each fixed
and frozen as new cases:

- Operator precedence. `A.NetName == 'X' || A.NetName == 'Y' && A.Layer == 'B.Cu'` selects only the X track
  on B.Cu in KiCad; read as in C it selected every X track. Without parentheses a rule could apply to items
  KiCad exempts, or miss items KiCad checks. Eight `precedence_*` cases.
- `Width` is a track and arc property. Pads and vias have none (`==` and `!=` false, relational comparisons
  see 0), where the engine gave pads 0 and vias their diameter: `A.Width < 0.3mm` missed every via, and
  `A.Width > 0.5mm` selected them. Five `width_non_track_*` cases.
- Numbers. KiCad compares for equality exactly; the engine allowed 1e-9 nm, so `A.Width == 1.001mm` matched a
  1.001 mm track and `!=` missed it (KiCad: the reverse, because 1.001 × 10⁶ is not a whole number in double
  precision). `.3mm` and `3e-1mm` are not numbers in KiCad and drop the rule; they were accepted. Eleven
  `number_*` cases.
- Courtyards. A footprint whose courtyard is drawn as two overlapping or touching outlines has their union as
  its courtyard in KiCad; the engine rejected crossing outlines, so the footprint had no courtyard and its
  rules never matched, without a message. Outlines that touch are now merged (outlines clear of each other
  keep even/odd filling), and a courtyard that still cannot be read (open, self-crossing, Bezier) is named in
  a warning by `tracemaker drc` and `tracemaker route` when a rule uses a courtyard function. Six
  `court_overlap_*`, `court_touch_*` and `court_disjoint_*` cases.
- Functions KiCad knows and TraceMaker does not evaluate. Dropping every rule with a symbol TraceMaker does not
  know also dropped rules KiCad applies: with `(A.NetName == 'X' && B.NetName == 'Y') || A.hasNetclass('a')`
  on a 0.8 mm clearance rule, KiCad and the engine before D80 report the X–Y pair, and the engine after D80
  reported nothing. Seventeen functions that KiCad 10.0.6 compiles (listed in §27) now keep their rule, with
  a warning; the call is taken as false as before D80. Symbols KiCad does not know still drop the rule
  (`hasExactComponentClass`, `isThroughVia`). KiCad's property names are an open set and cannot be told apart
  this way: a rule with a property TraceMaker does not evaluate is still dropped, with its warning. Twenty
  `kicad_function_*` cases.

Cost on a large board (jetson-agx-thor-baseboard, 1,125 footprints, its own rules plus a clearance rule with
`A.intersectsCourtyard('U*') || B.intersectsCourtyard('U*')`): the DRC took 61.7 s against 5.1 s without the
rule, because the rule is evaluated for every pair of items and each evaluation matched the selector against
every footprint and tested whole zone fills again. Each selector is now resolved once when the rules are
compiled (the scan over all footprints stays as the reference path, with a test that both agree), and a fill's
result is remembered as for the area functions: 6.0 s, same report.

The durable corpus is `tests/integration/rule_parity/`: `generate.py` creates boards and rules in the build
directory, `expected.json` freezes KiCad item/pair multisets (10.0.6 since the rejudge above), and `run.py` runs only TraceMaker DRC.
`ctest --test-dir build/macos-metal -R '^rule_parity$' --output-on-failure` needs neither KiCad nor routing.
To refresh the oracle deliberately, run:

```
python3 tests/integration/rule_parity/run.py build/macos-metal/src/app/tracemaker build/macos-metal/integration/rule_parity --rejudge --kicad-cli /opt/homebrew/bin/kicad-cli
```

Only rejudging may skip (77) when KiCad is absent; ordinary regression runs fail on missing binaries or
item/pair differences. Raw DRC reports and logs stay in the build directory.

The `[rules]` filter passes 95,985 assertions in 32 cases. Full ctest, excluding the two version-specific
`kicad_drc_parity` / `kicad_drc_broken_parity` tests, reports 0 failures out of 180 (177 passed; GPU Philox,
KiCad edit round-trip and catalogue sync skipped). C++ compilation has no warnings; Apple's pre-existing
duplicate-static-library linker warnings remain unchanged.

Real-board comparison against `origin/main`: Jetson clearance 3,604 → 2,040; every removed report is a
Via–Zone pair previously given 1 mm by `(hs_zone_clearance` through `A.Layer == B.Layer`. The new paired
corpus independently confirms that this rule does not match vias in KiCad. Multilayer zones also retain
their unset own-layer ID across fills, including inequality and wildcard comparisons. Vme-wren's complete
violation and unconnected-item arrays are unchanged (22,362 clearance, 21 shorting, 1 dangling track,
92 dangling vias); its six new `fromTo` warnings name length-only rules that DRC does not evaluate.
The 14-board DRC parity manifest stays 13/14 both before and after: the existing tiny-tapeout
`annular_width` mismatch is KiCad 16 / TraceMaker 0. That harness caps counts at 199; the custom-rule
corpus and the real-board item diffs do not.

Quick tier: the same 30 PCBench tier-A boards before and after, one portfolio thread, 60 s per board, six
parallel jobs: 30/30 clean and complete, zero added KiCad DRC errors, and every routed board byte-identical.
Runs are `bench/results/d80-before-quick` and `d80-final-quick` (not committed).

**Limits.** This is parity for the named corpus, not all of KiCad's expression language. Bezier courtyard
graphics, near-closed endpoint snapping and KiCad's small courtyard deflation tolerance are not covered;
a courtyard that cannot be read matches nothing and is warned about by footprint reference.
Courtyard arcs/circles use the existing 5 µm-sagitta polygonization; containment is exact against the represented
copper cores and contours. Circle/polygon/wildcard courtyard behaviour has direct engine tests but no dedicated
CLI corpus case. Positional router enforcement and zone refill are unchanged.

## 34. The router obeys positional disallow rules (2026-10-08, D82)

**Before.** `disallow track` / `via` rules whose conditions depend on where the item is or what it is
(`insideArea`, `intersectsArea`, `enclosedByArea`, courtyard functions, `Width`, `Position_X/Y`, footprint
properties) were DRC-only (§27, §33). The router warned and routed through them. With two common rules on
eight PCBench boards (below), every routed board had KiCad `items_not_allowed` errors: 16 to 102 per board,
476 in all.

**What was built.**

- `RuleEngine::probe_disallowed` judges a new track or via the way `disallowed` judges board items: every
  custom rule in file order, the last match deciding (so `ignore` exceptions hold). The probe is the actual
  copper: shape, width, position, net and layers, and for a via its span, so `blind_via` and `buried_via` are
  told apart there (the per-net via-type switch keeps its conservative gate). `positional_disallow()` is set
  when such a rule names tracks or vias; without one, nothing below runs and routing is unchanged.
- Disallow keywords are compiled to bits. Area and courtyard functions resolve their name patterns once
  (`Condition::bind`), and an area's outline box rejects far probes before the exact test: the router asks
  these functions for every lattice point it visits.
- `Obstacles::segment_state`, `disk_state`, `via_state` and `via_state_span` (now with the via type) return
  "blocked" for disallowed copper. They are the exact checks of commit, escapes, diff-pair moves and legs,
  clean-up shortcuts, in-pad vias and failed-commit learning, so nothing the rules forbid is committed. A
  disk is judged on its copper (half the track width), not the lattice margin. A blind-via probe from the
  top layer to the bottom one is judged as a through via, which is what commit writes for that span:
  otherwise `disallow through_via` in an area was passed by the blind attempt that follows a blocked
  through via.
- Search: `point_state` treats the rules like fixed copper. A point is blocked when they forbid the track's
  copper there, and legal only "tight" when they forbid the disk with the lattice margin, so steps between two
  free points stay legal. `via_cost_at` judges the via itself (vias sit exactly on lattice points). These tests
  run outside the per-class obstacle caches, since a rule may name nets that share a class. Escape planning's
  track and via tests and its dog-bone reservations apply them too.
- The warning "the router does not avoid it" is gone; unparseable rules and unknown symbols are still warned.

Checking the result against KiCad found a parity bug in §33. KiCad's front and back courtyards are the
footprint's own sides, so for a flipped footprint `intersectsFrontCourtyard` means its B.CrtYd outline.
Footprint selectors containing `:` also match library ids. Both were fixed with five new corpus cases (D80).

**Results.** Without positional rules the output does not change. Every routed board was byte-identical to
`origin/main`: `quick` 30/30, `mid` 11/11 and `planes` 8/8 (the commands of doc 10 §4).

With rules, KiCad 10.0.3 judges the output with the board's `.kicad_dru`. Eight PCBench boards (Hangul,
d20, LogicBoxen, custom_cpu ALU, mechkeys, threeboard, Inkjet, scimpy) were routed at 3M work with:

```
(rule "no vias under ICs" (constraint disallow via) (condition "A.intersectsCourtyard('U*')"))
(rule "bottom clear under front ICs" (layer B.Cu) (constraint disallow track)
  (condition "A.intersectsFrontCourtyard('U*')"))
```

| | `origin/main` (rules ignored) | This change |
|---|---|---|
| KiCad `items_not_allowed` from routed copper | 476 (8 boards) | 0 |
| Clean boards (no added error, complete) | 0 | 3 |
| Mean completion | 97.6 % | 88.8 % |
| Router seconds, sum | 11.3 | 11.5 |

Part of the drop is work: the rules make routes longer, and at 20M work Inkjet, LogicBoxen and custom_cpu
ALU route completely and clean. The rest comes from the rules themselves. d20 stops at 249/282 (276/282
without the rules) and threeboard at 80/119, and their failures are reported boxed in. On d20 these are pins
whose only exits are vias under their own IC (the QFN-48 U8 and the 1 × 1 mm UDFN-6 parts); on threeboard,
pads of bottom-side ICs inside the courtyard that the rule closes on B.Cu.

A net-dependent area rule was also tested on multichannel_mixer (`planes` settings: 3M work, `--soft-zones`,
refill before judging). The rule `A.intersectsArea('auto-placement-area-*') && A.NetName != 'GND'` disallows vias:
22 → 0 `items_not_allowed`, 177 → 175 routed.

Tests: a net-specific area rule (SIG is kept out, GND still crosses on F.Cu), no vias in a courtyard, router
probes against the direct area and courtyard tests on a grid (prefilters and bound selectors), and
`route_rule_warnings`, which routes with an area rule and checks `tracemaker drc` finds no disallowed item.

**Limits.**

- The per-net layer masks still come from position-independent rules alone. A later positional `ignore` rule
  that re-allows a layer in one area is not seen there, so the net stays off that layer (conservative).
- Search tests lattice disks. A diagonal step between two legal points can still clip an area corner; the
  exact check at commit rejects it and learns the cells.
- Cost-to-go fields and the global router do not see positional rules. The fields stay lower bounds; the
  global router only guides. (`tracemaker escape` applies them since §35.)
- Courtyard geometry has no deflation tolerance (§33). One track on threeboard is 1.25 µm into a courtyard by
  TraceMaker's test and outside it by KiCad's, so TraceMaker is the stricter of the two.

## 35. Escape analysis: outstanding pins and the board's rules (2026-10-09, D83)

**Before.** `tracemaker escape` (§12) searched every pin whose net has another pad. It ignored copper that
already joins the net, the per-net layer and via limits of custom rules (§27) and the positional disallow
rules the router now obeys (§34), and it always treated zone fills as fixed. On stripped CM5 it reported 18
dead pins. They sit in the hand layout's channels through fills that a refill would redraw: in a fair test on
the fork (5M then 30M work, KiCad-refilled DRC), `--soft-zones` routing connected 17 of the 18.

**What was built.**

- Obligations: only pins whose net the existing copper does not complete are searched (`pins`). Pins whose
  net is already one cluster (pads, tracks, vias and fills, with the router's connectivity) are counted as
  `satisfied`. With `--soft-zones` a fill of 1 mm² or more that no pad touches keeps its net outstanding: it
  is a plane target in the router's plan (§26), which connects one pad of the net to it. The test for that is
  the planner's own (`zone_fill_area`, `kPlaneTargetArea`).
- Rules: each pin's search uses its net's track layers and via permission (`track_allowed`, `via_allowed`, the
  hole-rule veto) and the positional rules on the track's copper at each lattice point and on the via. The
  fixed-copper code caches stay per class; rule verdicts are memoised per pin. A dead pin that escapes with
  the rules ignored is reported "walled in by custom disallow rules (it escapes without them)".
- Options as `route`: `--soft-zones` (fills do not block) and `--keep-vias-off-pads` / `--vias-off-pads-below`.
  `--flow` applies the same rules to its channel and via tests. Rule warnings are printed as `route` prints
  them.
- Output: the summary line names the analysis lattice; JSON adds per-part and total `satisfied` and a `domain`
  object (lattice, margin, window, zone and via options). `pins` and `dead` keep their meaning for
  `bench/run.py` and `bench/feasibility.py`.

"Dead" still means no escape on the analysis lattice inside its window. The breadth-first search is complete
there, but an off-lattice path can exist. A pin that escapes alone can still lose its channel to its
neighbours.

**Results.** Without rules and with fills hard, the 30 `quick`, 11 `mid` and seven of the eight `planes`
boards give the same dead pins as before. (The eighth, "sonde xilinx", has a space in its path that the
comparison script did not handle.) RoyalBlue54L-Feather has two pins whose nets are already complete; they
move from `pins` to `satisfied`.

| Board | Dead, fills hard | Dead, `--soft-zones` |
|---|---|---|
| CM5 (stripped, `bench/prepare_demos.py CM5_MINIMA_3`) | 18 of 340 | 0 |
| RoyalBlue54L-Feather | 19 of 236 | 2 |
| StickHub | 1 of 46 | 0 |
| plane_smd | 1 of 30 | 1 |

With §34's "no vias under ICs" rule, kitspace_d20's U8 exposed pad (pin 49) is "walled in by custom disallow
rules". The router reports the same pin boxed in.

**Limits.**

- One lattice (0.04 mm), the neck-down width and the smallest through via. Blind and micro vias, the per-run
  `--no-tracks-on` limits and the router's off-lattice escape rungs are not modelled.
- Satisfied means the net is complete. A pin whose cluster still has to reach another cluster is searched from
  its pad, through its own net's copper.

## 36. Zone refill in the engine (2026-10-09, D85)

**Before.** With `--soft-zones` the router cuts through fills and trusts the stale polygons for connectivity (§26).
KiCad refills before judging, and the refill undoes connections the router counted; the router neither saw nor
repaired that. Measured on the `planes` set at 3 M work units with `--soft-zones` (KiCad 10.0.3, unconnected items):

| Board | Router gap | KiCad, stale fills | `tracemaker drc`, stale | KiCad `--refill-zones` | `tracemaker drc`, KiCad's fills |
|---|--:|--:|--:|--:|--:|
| RoyalBlue54L-Feather | 52 | 52 | 52 | 63 | 63 |
| StickHub | 2 | 1 | 1 | 15 | 15 |
| complex_hierarchy | 1 | 1 | 1 | 17 | 17 |
| interf_u | 23 | 23 | 23 | 33 | 33 |
| multichannel_mixer | 0 | 0 | 0 | 4 | 4 |
| pic_programmer | 4 | 4 | 4 | 7 | 7 |
| plane_smd | 1 | 1 | 1 | 1 | 1 |
| sonde xilinx | 0 | 0 | 0 | 2 | 2 |

The engine's connectivity already agreed with KiCad's on all 16 boards (also routed with fills fixed); only the
fill polygons were missing. The last column uses KiCad's `ZONE_FILLER` fills with teardrop zones removed, which is
what `kicad-cli --refill-zones` does first: `ZONE_FILLER_TOOL::FillAllZones` calls
`TEARDROP_MANAGER::UpdateTeardrops` with a forced full update, so a teardrop whose track was deleted disappears (a
rebuilt teardrop joins a pad to a track that already touches it). What the refill broke: the GND pour split into
islands by new tracks (StickHub), through-hole pads cut off from their plane (interf_u, pic_programmer, sonde
xilinx), and tracks ending in a deleted teardrop (complex_hierarchy, RoyalBlue; also with fills fixed: 0 → 18).

**What was built**

| Part | What |
|---|---|
| `geom/clip` | Polygon union, difference, intersection, even-odd union, offsets and point tests over Clipper2 (int64, deterministic for a given input order); `fracture` joins holes to the outline by zero-width bridges, as KiCad writes `filled_polygon` |
| `drc::refill_zones` | Follows `ZONE_FILLER::fillCopperZone` per copper zone and layer, layers in parallel and zones by priority: outline within the board outline (not clipped when the outline does not close at 10 µm, as KiCad); thermal gaps around same-net pads with a thermal connection (pad, then footprint, then zone setting; `thru_hole_only` thermal for plated holes only; coincident pads once); every other-net pad, track, via, hole, copper graphic, board edge, pour keep-out and higher-priority zone of another net knocked out at the `RuleEngine` clearance + 0.5 µm; four spokes per thermal pad kept when their end lands in the fill after the minimum-width test or in another pad's spoke; minimum-width pruning (deflate and re-inflate by `min_thickness/2 − 1 µm`, blobs dropped); same-net higher-priority zones' areas removed; islands whose cluster holds no pad removed by `island_removal_mode`, unless every island of the zone is one. Teardrop zones lose their fill; a hatched zone is not redrawn: its stored fill is kept where the solid fill for the current copper still is (warning), so copper a refill knocks out carries no connection |
| Reader | Zone `connect_pads` mode, `min_thickness`, `thermal_gap`, `thermal_bridge_width`, `island_removal_mode`, `island_area_min`, hatch mode and uuid; pad and footprint `zone_connect`, pad thermal gap, width and angle. Copper layers with free names (`Gnd.Cu`, `Vcc.Cu` in KiCad 5 boards) get their canonical name from the ordinal; before, they were not copper at all |
| `tracemaker drc --refill-zones` | Judges the refilled board, like kicad-cli's flag; the fills are not written |
| Route with `--soft-zones` | Refills in memory after routing, reports `unconnected_after_refill` and a `refill` object in the summary, then routes what is unconnected once more with the refilled fills as fixed copper (pass 2, the same work budget as the first pass, within what is left of `--time`); the repair's copper is kept only if the count after refilling again is lower (rule 4). `refill.warnings` lists what the refill could not model. `--no-refill-repair` only counts. The exit code follows the count after the refill |
| Connectivity | `compute_connectivity` can test items against fills through the fill edge index (`geom::PolygonIndex`); the DRC and the refill use it, `drc --linear-zones` keeps the linear reference (ctest `drc_zone_index`, now on refilled boards too). A refilled keyboard (mechkeys LFK78, 478 islands) went from 39 s to 1.7 s |
| `bench/refill_parity.py` | Unconnected items per net, `tracemaker drc --refill-zones` against `kicad-cli pcb drc --refill-zones` |

**Results**

- **Parity.** The 16 routed `planes` boards above (soft and fixed fills): unconnected items equal KiCad's on all
  16. PCBench, 200 of the original boards (`raw.kicad_pcb`) with zones, evenly spaced in name order: the same unconnected items per
  net on 191. Eight of the nine others are KiCad 5 files whose zones share one id (`tstamp 0`): KiCad breaks
  priority ties by that id, and its own fills of those boards empty or misattribute zones (on OpenMPPT four zones
  fill alone but are empty together). The refill warns on such boards. The ninth has a `target` on Edge.Cuts,
  which the reader does not read. Engine refill and judge take 0.02-2 s per board, `kicad-cli` 1.4-8.7 s.
- **No DRC errors from the fills.** On the refilled `planes` boards `tracemaker drc` finds the same clearance and
  edge errors as KiCad and none on new fills.
- **Repair.** `planes`, 3 M work units, `--soft-zones`, KiCad after `--refill-zones`:

| Board | Soft 3 M | Soft 6 M | Soft 3 M + repair 3 M |
|---|--:|--:|--:|
| complex_hierarchy | 3 | 3 | 0 |
| RoyalBlue54L-Feather | 54 | 46 | 51 |
| multichannel_mixer | 4 | 4 | 3 |
| StickHub | 15 | 14 | 8 |
| sonde xilinx | 2 | 2 | 0 |
| pic_programmer | 7 | 7 | 4 |
| interf_u | 33 | 26 | 28 |
| plane_smd | 1 | 1 | 1 |
| Total | 119 | 103 | 95 |

  The engine's `unconnected_after_refill` equals KiCad's count on all eight. No board gained a DRC error
  (StickHub's `solder_mask_bridge` count moves between runs: doc 10 §4, judge noise). On RoyalBlue and interf_u more
  soft work beats the repair; the repair spends the same budget as the first pass whatever is left.
- **Unchanged elsewhere.** `quick` (30 boards) and the `planes` set with `--no-refill-repair` are byte-identical
  to `origin/main`. In `mid`, oskirby_logicbone changed: its six inner layers are named `Gnd1.Cu` … `Sig2.Cu`, so
  the router saw a two-layer board before; it now routes on eight, 966 → 936 of 1188 at 20 M work units (KiCad:
  completion 55.5 % → 49.5 %, no added errors). The other ten are byte-identical.

**Limits**

- Not modelled: KiCad's iterative refill (lower zones reclaim area from removed islands), corner smoothing,
  hatched fills, copper text knock-outs, net ties, courtyard and physical clearance rules, custom
  `zone_connection` / thermal rules (warned), Margin line widths, and the islands-outside-the-board test (fills
  are clipped to the outline instead).
- Fills are for judging only: the routed board still carries the stale fills (doc 08 asks for a refill before
  judging).
- The repair is one pass with the first pass's work budget and the rest of the time limit (none when the first
  pass used it up); no portfolio variant is chosen by its count after the refill.

## 38. Buses (2026-10-11, D91)

**Before.** The router knew pairs (§15) and single nets with a length rule, but not a group of nets that belong
together: a data or address bus, a parallel display interface. Its members were routed wherever the order put
them, each by its own cheapest path, so one took a via its neighbour did not need, one went round the other side of
a mounting hole, and their lengths differed by millimetres.

**Status: experimental.** The generated bus boards meet their skew limits, but six real boards routed slightly
fewer connections with the options on (957 → 952 at 100 M work, 886 → 853 at 20 M), and `--buses` groups some
indexed nets that are not a bus. The options stay opt-in until real boards gain from them.

**What was built** (off by default: without the options the output is byte-identical, `quick` 30/30).

| Part | What |
|---|---|
| Groups from the command line (`route/bus.cpp`, `bus_from_option`) | `--bus NAME=PATTERN`, repeatable. The pattern matches whole net names as KiCad writes them (sheet path included: `/D0`, `/MEM/DQ3`). `[N-M]` or `[N..M]` is an index range and matches any decimal number in it (`/D[0-15]`, zero-padded indices too); `*` and `?` are KiCad's net-class wildcards; everything else in a pattern with a range is literal (`A[[0-15]]` for nets named `A[0]`). A pattern without a range goes to KiCad's net-class matcher unchanged (`model::pattern_match`: wildcards, or a full regular expression, `/SPI_(SCK|MOSI)`). Members are ordered by natural sort (`D2` before `D10`); only nets with pads count; fewer than two is an error. A net named by two groups is routed with the first (warning) |
| Detection (`detect_buses`, `--buses`) | Nets whose names differ only in a trailing decimal index of at most four digits (`D0`, `A[3]`, `/MEM/DQ12`), four or more of them with pads on the same two components. One stem may give several groups (`DQ0-7` to one memory, `DQ8-15` to another: `STEM(REF-REF)`). KiCad's generated names (`Net-(…)`, `unconnected-…`) and names without a letter are never grouped. A detected group that shares a net with a `--bus` group is dropped. The groups are listed in the log before routing and in the summary |
| Skew limit (`bus_rule_skew`, `app::resolve_buses`) | A custom `skew` rule of the board applies to a group when every member matches one (the smallest maximum; `RuleEngine::net_constraint`, the call the pair code uses), e.g. `(rule "bus" (constraint skew (max 0.8mm)) (condition "A.NetClass == 'BUS'"))`. Rules marked `(within_diff_pairs)` do not count (the project reader now keeps such bracketed flags in `Constraint::items`). Otherwise `--bus-skew-mm X` gives the `--bus` groups a limit. With neither there is no length matching, only grouping: a limit is never invented, and detected groups get one from rules only |
| Bundles and lanes (`Router::Impl::order_buses`) | The connections of a group between the same two components form a bundle (the planner's spanning tree gives a fan-out or a fly-by chain one bundle per component pair). All of a bundle's connections are turned the same way round and ordered along the pad row at one end (the wider of its x and y extents), so neighbouring lanes are neighbouring pins and do not cross. A bundle is routed as one block, lane by lane from one side, at the place the router's order gave its first connection; with `--bus-first` before everything else (after `--first-nets`). Restarts keep the blocks together |
| Following (`route_bus_lane`, `bus_note`) | The first lane routed leads: it is an ordinary search. Each later lane is first searched inside a guide around the nearest lane already routed as a lane: cells within reach of that lane's tracks on the same layer, and on every layer around its vias and around the new lane's own pads; everything else is blocked (`bus_mode` 2 in `cell_cost`). The reach is the distance between the two lanes' pads (at most 5 mm) plus two track pitches and a via. So the lane runs beside its neighbour and can change layer only where the neighbour did. The search is the router's A* with its exact commit; it is cut off at four expansions per lattice point of the guide (at least 50,000), and a rejected commit is retried twice. The result is kept only if it has the neighbour's layers in the same order and as many vias, and its detour (length beyond the octilinear distance of its pads) exceeds the neighbour's by no more than half the pad distances plus 0.5 mm plus 2 % of the neighbour's length; otherwise it is taken back |
| Leaving the bus | A lane that cannot be routed that way is routed like any other connection (strict, escapes, neck-down, negotiation), with the guide as a preference only: a quarter pitch per step outside it. If it still has the neighbour's layers and vias it is a lane, and later lanes may follow it (this is how a bundle passes an obstacle on both sides); if not, it has left the bus: it stays routed and is reported, and no lane follows it. No connection stays unrouted for the sake of its group |
| Ripping | Nets outside the groups never rip a group's copper (as with `--first-nets`; `commit`); members rip each other as usual. A lane ripped is no lane any more. The via clean-up (`optimize_vias`, `lns_vias`) leaves lanes alone, since removing one member's vias would break the equality; smoothing keeps vias and layers and runs on them |
| Length matching (`tune_bus_skew`) | In the clean-up, per group with a limit: the members with every connection routed are measured as KiCad's `skew` check measures them (track length of the net plus the stackup height of each via between the layers it joins; `route/bus.hpp`, `via_heights`, `net_via_length`), and each one shorter than the longest by more than half the limit gets meanders up to between half the limit below the longest and the longest. It is the length-tuning code (`tune_net`, §15) with two additions for bus members, both off elsewhere: every track is tried, longest first (the lanes beside a member leave its longest track no room), and the last meander is cut to the length still missing (the fixed heights step by 0.5 mm, too coarse for a 0.5 mm limit). Each meander is checked exactly |
| Portfolio | Between variants with equal completion the one with fewer connections off their lanes wins, then vias and length as before (0 on both sides without buses) |
| Report | `--json`: a `buses` array, per group `name`, `source`, `members` (`net`, `routed`, `followed`, `length_mm`, `via_length_mm`, `vias`, `layers`; `length_mm` includes the vias), `routed`, `followed`, `left`, `reference_vias`, `reference_layers`, `vias_min`/`vias_max`, `length_min_mm`/`length_max_mm`, `skew_mm`, `skew_limit_mm`, `skew_source`, `within_skew`; and `bus_lanes` / `bus_left`, the router's own count of connections routed as lanes and off them. The array is measured on the board as written (`route::measure_bus`), not taken from the router's book-keeping |
| Measurement (`tracemaker buses BOARD [--bus …] [--buses] [--bus-skew-mm X] [--json F]`) | The same numbers for any routed board. A member is routed when all its pads are joined (the router's connectivity). The reference is the via count and layer set most routed members share (the first member's among equals); `followed` members have exactly that, `left` are routed otherwise. Length is what KiCad's skew check sums: tracks and arcs, plus per via the stackup between the outermost layers on which a track of the net ends in the via (outer copper in full, inner copper by half, as `BOARD_STACKUP::GetLayerDistance`; KiCad's default stackup when the board has none; nothing when the project turns "use height for length calculations" off). Skew is the longest minus the shortest routed member. Exit 4 when a group with a limit is over it or not completely routed |
| Events (doc 13) | `bus` (name, nets, per bundle), `bus_lane` (connection, net, `leader` / `followed` / `left`) and `bus_skew` (skew and limit after tuning) |

Work is counted as for every search (expansions and flood cells of the guided and the ordinary attempts); building
a guide and placing meanders are geometry and are not charged, like the pair legs' set-up and the existing tuning.
Nothing is random and no hash container is iterated: the same job gives the same bytes at any thread count
(`bus_route` test, 1, 2 and 4 threads).

**Fixtures.** There is no public bus benchmark. `bench/make_bus_boards.py OUT` writes seven small KiCad 10 boards
(s-expressions written directly, identifiers from a counter, so the bytes are the same everywhere) and a manifest
with each board's bus options; `bench/bus_eval.py --fixtures OUT` routes each without and with them at the same
budget, measures both written boards with `tracemaker buses` and judges both with KiCad.

| Board | What it is |
|---|---|
| p2p8 | 8 bits between two facing ICs (mirror-image pin columns) on two layers; a mounting hole and two capacitors stand in the channel and the ICs are offset. `--bus 'DATA=/D[0-7]' --bus-skew-mm 0.5` |
| p2p16 | 16 bits from an IC on the front to one on the back (footprint mirrored): every lane needs a via. Limit 1 mm |
| lcd | 12 bits from a QFP to a through-hole header on four layers; a rule area across the board takes no tracks or vias on the front layer. `--bus 'LCD=/LCD_D*'`, limit 1 mm |
| spi | SCK, MOSI, MISO from a controller to three devices in a column (four pads per net) on two layers. Limit 2 mm |
| flyby | 8 address lines from a controller through three memories in a row, four layers. Limit 0.25 mm |
| skewrule | p2p8 with a net class `BUS` and a `.kicad_dru` rule `skew (max 0.8mm)` on it; routed with `--buses` only |
| twist | p2p8 with the pin order reversed at one end, so every lane must cross every other. Not in the gate |

**Results** (one variant, `--work 20000000`, `--no-gpu`, KiCad 10.0.6 DRC; `bench/bus_eval.py`; "left" = routed
members off the group's reference vias and layers; skew as `tracemaker buses` measures it on the written board, via
height included).

| Board | Routed off → on | Vias per member off → on | Left off → on | Skew mm off → on (limit) | KiCad added errors off → on |
|---|---|---|---|---|---|
| p2p8 | 16 → 16 / 16 | 2–2 → 2–2 | 0 → 0 | 0.820 → 0.217 (0.5) | 0 → 0 |
| p2p16 | 24 → 24 / 24 | 1–1 → 1–1 | 0 → 0 | 2.665 → 0.289 (1) | 0 → 0 |
| lcd | 16 → 16 / 16 | 1–1 → 1–1 | 1 → 0 | 4.033 → 0.268 (1) | 0 → 0 |
| spi | 20 → 20 / 20 | 0–6 → 0–6 | 2 → 2 of 3 | 9.092 → 0.210 (2) | 0 → 0 |
| flyby | 34 → 34 / 34 | 0–2 → 0–4 | 1 → 1 of 8 | 0.560 → 0.063 (0.25) | 0 → 0 |
| skewrule | 16 → 16 / 16 | 0–2 → 2–2 | 2 → 0 | 3.946 → 0.214 (0.8, rule) | 6 → 0 |
| twist (not gated) | 16 → 16 / 16 | 2–2 → 2–2 | 0 → 0 | 9.263 → 0.347 (1) | 0 → 0 |

Every group is within its limit with the options and none without; completion is equal; where no member left, the
via count per member is equal (p2p8, p2p16, lcd, skewrule, twist). The six errors on skewrule without the options
are KiCad's own `skew_out_of_range` for the board's rule, so KiCad's judgement of the rule agrees with the
measurement on both rows. On spi and flyby members leave. On spi the lanes between two devices with the same pin
order must cross (SCK has to pass the MOSI and MISO pads of one device, MISO the SCK and MOSI pads of the next),
so they cannot all have the same vias unless every one hops. On flyby one member takes four vias in the router's
order and none with `--bus-first`; why was not examined. The members are matched in length all the same, vias
counted: on spi the member without vias gets 9 mm of meanders against the one with six. With a 2 mm `skew` rule
beside the routed spi board and a 0.25 mm one beside flyby, KiCad's DRC reports no `skew_out_of_range`; measured on
track length alone (as this was first built) the same check reported 2 and 7.

With `--bus-first` (same boards and budget): p2p8 0–0 vias per member (6 vias on the board instead of 20), skew
0.061; skewrule 0–0, 0.054; flyby 0–0, nobody left, 0.050; p2p16 and lcd unchanged; spi 0–6 with two left, 0.237;
twist 0–2 with two left, 0.300. All gated groups within their limits, no added errors. Routed first, a bus gets the
straight way across the channel; in the router's order the two power connections between the capacitors, which
cross the channel, are there before it, and all eight lanes hop over them with the same two vias each (as they do
without the options).

**Real boards** (information, not a gate; PCBench, `--buses`, one variant, no skew limit: none of these boards has
a skew rule). Six boards with memory buses, at 20 M and at 100 M work:

| Board | Groups (nets) | Routed off → on, 20 M | Routed off → on, 100 M | Bus nets routed / on the reference / left, 100 M off → on | Vias on bus nets, 100 M off → on |
|---|---|---|---|---|---|
| lattice-ice40-hx8kevb-sdram | 6 (38) | 92 → 81 / 115 | 115 → 110 | 38 / 25 / 13 → 37 / 26 / 11 | 39 → 39 |
| srambo_1 | 4 (34) | 114 → 114 / 132 | 122 → 127 | 26 / 20 / 6 → 34 / 20 / 14 | 37 → 49 |
| NRC2016_z80 | 4 (32) | 276 → 269 / 290 | 289 → 290 | 31 / 10 / 21 → 32 / 16 / 16 | 50 → 67 |
| NRC2016_banked_ram | 2 (23) | 120 → 121 / 148 | 133 → 129 | 14 / 5 / 9 → 14 / 6 / 8 | 36 → 20 |
| FlashProgrammer | 3 (20) | 141 → 139 / 141 | 141 → 141 | 20 / 15 / 5 → 20 / 16 / 4 | 10 → 5 |
| ice40_stm32_fsmc_test_pcb | 4 (35) | 143 → 129 / 183 | 157 → 155 | 21 / 14 / 7 → 21 / 15 / 6 | 30 → 22 |
| Total | 23 (182) | 886 → 853 / 1009 | 957 → 952 | 150 / 89 / 61 → 158 / 99 / 59 | 202 → 202 |

KiCad finds no added error on any of the 24 boards written. At 100 M the grouping is about neutral in completion
(three boards down by 2 to 5 connections, two up by 1 and 5) and moves ten more bus nets onto their group's vias
and layers; groups routed completely rise from 14 to 17 of 23, groups with equal vias per member from 1 to 5. At
20 M these boards are still in their first pass when the budget ends (hx8kevb: 12 M of the 20 M went into failed
strict searches without buses, 14 M with), and a bundle brings its long connections forward to where its first one
stood, so the budget ends earlier in the order: 33 connections fewer in all. Four large boards of the `mid` set
with detected buses (decelerator4030, Aleste-520EX, EEZ DIB MCU, sbc; 20 M) say little about buses: 301 → 281 of
1009, 1001 → 955 of 1616, 470 → 470 of 634 and 310 → 310 of 410 connections, no added errors, and the budget ends
before the buses are reached. On three of them no bus net is routed with or without the options (the last two have
the same connection and via counts in both runs); on Aleste-520EX 32 → 31 of 150 bus nets are, and four of its 24
groups completely in either run.

**Tried and dropped.**
- *Buses first by default.* Better on the fixtures (above), ruinous on dense boards at a fixed budget: the lanes out
  of a ball grid are the hardest connections, each failed strict attempt costs up to the per-search limit, and with
  all of them in front the four `mid` boards routed 11 / 1009, 40 / 1616, 11 / 634 and 1 / 410 connections at 20 M.
  It is `--bus-first` now, for boards where the buses are what matters.
- *Accepting any path found inside the guide* (with two pitches per step as the penalty afterwards): a lane then
  winds through the guide or hops over what is in its way rather than pass the mounting hole on its own side.
  p2p8's eight lanes came out at 25, 25, 25, 28 (with two vias), 37, 44, 49 and 54 mm, and the group at 23.6 mm of
  skew, since the short lanes had no room for that much meander. A limit of an eighth of the neighbour's length plus
  twice the pad distances changed nothing (each lane was only a little longer than the one before); the layer test,
  the detour limit and the quarter-pitch penalty did.
- *Ripping a group as a whole* when another net rips one lane, and *no protection at all* (six boards, 100 M): 936
  and 947 connections against 952 with protection, and 92 and 94 bus nets on their reference against 99.

**Limits.**
- Following is by neighbour, not by plan: the leader takes its own cheapest path, and nothing reserves room for the
  lanes behind it or picks the layer pattern that suits the whole bundle. A bundle whose lanes must cross (same pin
  order at both ends of a chain, reversed order) loses members or gives every lane the same pair of vias.
- "On the bus" in the report is the via count and the set of layers per net, measured on copper; the router's own
  test per connection also compares the order of the layers. For nets with several connections the two can differ.
- Length follows KiCad's skew check (tracks plus via height) but not its last details: pad-to-die lengths are not
  read, and KiCad shortens a track end inside a pad or a via (differences of a few micrometres on the fixtures). KiCad
  compares every net its rule matches; a group is compared within itself, so a rule matching more nets than the group
  can still fail in KiCad. The pair skew (§15) is still track length only.
- Meanders need room: a member squeezed between its neighbours and an obstacle stays short, and the group is then
  reported over its limit. There is no spreading of lanes to make room, and no limit in picoseconds.
- No pair-style coupling or fixed spacing between lanes; a group is not planned by the global router; `length` rules
  on a group are tuned per net as before, not as a group.
- Bus options are not in the Python bindings' keyword list (`RouteJob` carries them).
## 39. After-route passes: power widening, style metric, stitching vias (2026-10-11, D92)

Three things that happen after routing has finished. Each is its own small module in `src/passes/`, attached in
`app/route_job.cpp` after the soft-zone refill repair and before the board is written. All are off by default (the
style metric only measures), deterministic, and count their work in exact checks with a budget (`--pass-work`,
default 2 M per pass; wall time does not stop them). The two that change copper do it through the router's own
obstacle model (`route::Obstacles`, the check the router commits with), touch only copper of this run, and are
transactions (rule 4): the job counts unconnected items per net with and without the pass's result (after an
in-engine refill when fills are soft or vias were added) and drops the result if any net has more than before
(per net, so that a connection made in one net cannot hide one broken in another). Without the options the routed
board is byte-identical to before.

### 39.1 Power track widening (`--widen-power`)

**Which tracks, and how wide.** Nothing is invented: a track is widened only towards a width something stated.

| Source | Target | Applies to |
|---|---|---|
| Net class (`source: "net class"`) | The class's track width (raised to the board minimum) | New tracks narrower than that: the router's neck-down (§6, §14), on every net except coupled pairs |
| `--widen-net PATTERN=WIDTH_MM` (repeatable; KiCad wildcards on the net name) | The stated width | Every new track of the matching nets |
| Component rules (`--component-rules report`, `soft` or `on`) | The width a `width_for_current` rule computed (IPC-2221 from the rule's current, or its `min_width_mm`; doc 15 §16) | The nets the rule binds, as `synthetic_net_classes` gives them: instances at the apply threshold, nets in the Default class only |

The largest stated target of a net counts, and a stated target replaces the class width for that net, also when
it is narrower: the pass never widens beyond a stated target. Impedance widths are not targets (a
controlled-impedance track must not be wider than computed either), and the halves of coupled pairs
(`--diff-pairs`, USB pairs from component rules) are skipped: they were routed at their pair width on purpose.
With `--component-rules on` the computed widths are already the nets' class widths, so the first row restores
whatever the router had to neck.

**Method.** Per track, in commit order, nets with a stated target first. The candidates are the goal and seven
evenly spaced widths below it, down to (not including) the track's own width, rounded down to a micrometre; the
widest whose `segment_state` is free against everything else is kept and entered in the obstacle model at once, so
later tracks are checked against it. "Everything" is what the router checks: other nets' copper at the pair's
clearance, holes, the board edge, keep-outs, mask openings, custom rules (positional `disallow`, a rule's maximum
`track_width` caps the goal), and the neighbours of the net's own pads. No other copper is moved or narrowed in
this version, so a track in a tight channel simply stays as it is.

**Junctions.**

- *Wide meets narrow.* Tracks are stadium shapes: both keep their round caps at the shared end point, the copper
  stays joined, and the wide cap is part of what the exact check tested. No taper is added.
- *Via.* A track that ends on a via of its net (at the via's centre, on a layer the via spans) is widened to the
  via's diameter at most, so the track never sticks out around the via.
- *Pad.* A track that ends inside a pad of its net is widened to the pad's smaller side at most: the neck stays at
  the pin, as a designer draws it, and the wide copper starts at the first bend or junction away from it. Such a
  track counts as "at target" in the report when it reaches that size.
- Locked tracks and every track that was on the board before the run keep their width.

**Report.** One log line per widened net and a `widen` object in the JSON summary: per net the target and its
source, tracks below the target, tracks widened, tracks at target, widened length, and the length at each width
before and after (`before_mm_by_width`, `after_mm_by_width`); in total the checks done and whether the result was
kept.

**Limits.** One width per track as the router cut it: a long track with one tight spot stays narrow along its
whole length (splitting a track at the obstacle is not done). Tracks are not moved to make room. A custom rule's
maximum `track_width` caps what this pass does (`RuleEngine::track_width` returns it, and the in-engine DRC reports
tracks above it); the router itself still routes at the class width where such a rule asks for less. Inner-layer
widths from IPC-2221 (wider than outer) are reported by the component rules but the pass uses the one width a net
class holds. With `--soft-zones` the widened tracks are judged like the routing itself, after a refill.

### 39.2 Style metric

How tidy the copper is. It only measures: nothing in the router or the clean-up optimises it yet. Counts and
angles are integer arithmetic on nanometres (cross and dot products); lengths are each segment's `hypot` rounded
to the nanometre and summed as integers. Nothing depends on the order of the input or on threads, so the numbers
are the same at every thread count.

A **connection** is copper of one net between two ends; an end is a pad of the net, a junction (three or more
track ends, or a track end on the middle of another track) or a loose end. A via with exactly two track ends is
passed through. Collinear pieces of one width are joined first (the router commits a straight run as several
steps), so a **segment** is a maximal straight run.

| Measure | Definition |
|---|---|
| `bends`, `bends_per_connection` | Direction changes between consecutive segments on one layer (not across a via, not at an arc) |
| `acute_angles` | Bends where the two segments enclose less than 90° (the direction turns by more than 90°) |
| `stair_steps` | Maximal runs of three or more consecutive segments on one layer, each shorter than four track widths, whose bends turn alternately left and right. A single Z-shaped jog (two short segments) is not one |
| `short_segments` | Segments shorter than their own width |
| `off_angle_segments` | Segments more than 0.1° away from the eight octilinear directions |
| `detour_ratio` | Copper length over the shortest octilinear path between the connection's ends (max + (√2 − 1) · min of the two offsets); per board the two sums are divided (`detour_length_mm`, `detour_octile_mm`), closed loops excluded |
| `long_detours` | Connections longer than 1.5 times that path |
| `arcs`, `vias`, `segments`, `connections`, `length_mm` | Counts; arcs are measured by their length and not judged for bends or angles |

The patterns follow what tscircuit's open-source linters flag (`pcb-trace-linter`: segments off the 45°
directions; `check-ugly-traces`: removable jogs and off-angle segments; both MIT). They were read for which
patterns to count; no code was taken.

**Where.** `passes::measure_style` runs on the new copper of every route job: the JSON summary has a `style`
object. `tracemaker style BOARD [--json FILE] [--connections]` measures every track, arc and via of any board
(Freerouting's output, a designer's routing); on a board that had no tracks before routing both give the same
numbers. `bench/run.py` records `style` per board, `style_ref` for the fixture's own routing where it has one
(PCBench `processed.kicad_pcb`), and the totals in `summary.json` and `report.md`; `bench/compare_runs.py` lists
the boards whose metric changed. Informational everywhere, never a gate.

**Does it tell routings apart?** `quick` set, 1 M work units, `--variants 1`: TraceMaker's new copper against the
boards' reference routing (all 30 have one). TraceMaker routed 89.2 % of the connections on average at this
budget, the references are complete and include the copper of poured nets, so the counts are not for the same
copper; the per-connection figures and the ratios are comparable.

| Routing | Connections | Bends per connection | Acute angles | Stair-steps | Short segments | Off-angle segments | Detour ratio | Long detours |
|---|--:|--:|--:|--:|--:|--:|--:|--:|
| TraceMaker | 2,447 | 0.62 | 0 | 29 | 268 | 409 | 1.031 | 23 |
| Reference routing | 3,568 | 1.23 | 75 | 209 | 1,673 | 599 | 1.103 | 67 |

Per board (TraceMaker / reference), the first eight of the set:

| Board | Bends per connection | Acute | Stair-steps | Short segments | Off-angle | Detour ratio |
|---|--:|--:|--:|--:|--:|--:|
| 655_testboard | 0.87 / 1.96 | 0 / 4 | 0 / 42 | 11 / 284 | 5 / 109 | 1.094 / 1.233 |
| C-BISCUIT_buck-reg-5v | 0.43 / 0.85 | 0 / 0 | 0 / 1 | 8 / 26 | 5 / 0 | 1.031 / 1.039 |
| CubeSAT-Reaction-Wheel_Edison_Motor_Servo | 1.00 / 2.75 | 0 / 0 | 0 / 0 | 0 / 0 | 0 / 0 | 1.000 / 1.065 |
| Dekada_dekada_TopoR_curves | 0.23 / 1.62 | 0 / 64 | 0 / 105 | 45 / 556 | 0 / 402 | 1.015 / 1.003 |
| ESP8266-MQTT-battery-monitor-hw_battery-monitor | 0.25 / 1.05 | 0 / 0 | 0 / 4 | 22 / 33 | 38 / 7 | 1.010 / 1.090 |
| HES-V2__autosave-hes | 0.51 / 0.83 | 0 / 0 | 0 / 0 | 1 / 4 | 0 / 5 | 1.002 / 1.043 |
| Hangul-Clock_Hangul | 0.80 / 1.60 | 0 / 0 | 0 / 12 | 0 / 86 | 16 / 0 | 1.000 / 1.118 |
| Hardware_Playground_rpi_zero_ws2812 | 0.61 / 0.70 | 0 / 0 | 0 / 2 | 7 / 70 | 4 / 1 | 1.018 / 1.031 |

The metric separates them: the any-angle TopoR board stands out in off-angle and short segments, hand routing has
about twice the bends per connection, and TraceMaker's own weak spots show too (409 off-angle segments, the
off-lattice pad entries of §5, and 29 stair-steps, 27 of them on one board). Widths matter to two measures: after
`--widen-power` the same polylines have more short segments and stair-steps, because both are counted in track
widths.

**Limits.** A T-junction on the middle of a track ends the incoming connection but does not split the track it
lands on. Connections are pieces between junctions, so a net's detour is judged piece by piece, not pad to pad
along the whole tree. The metric says nothing about spacing, symmetry or how pairs look.

### 39.3 Stitching vias (`--stitch-vias planes|return|all`)

Vias of an existing net, added where they tie that net's copper on two or more layers together. Two uses:

- **Plane stitching** (`planes`): for every net with solid fills on two or more layers, a via at each site of a
  grid (`--stitch-pitch`, default 5.08 mm, the catalogue's `general_high_speed.stitch_via_within_mm`; one grid for
  the board, anchored half a pitch inside the outline's bounding-box corner) where the via's pad lies wholly inside
  that net's fill on at least two layers. A site with a via or plated through-hole pad of the net within half a
  pitch is already tied and gets none.
- **Return-path stitching** (`return`): every via of a signal that needs a reference plane wants a via or plated
  pad of the plane's net within a distance, centre to centre. Signals come from component rules of kind
  `reference_plane` (doc 15 §4; with `--component-rules report`, `soft` or `on`, instances at the apply threshold:
  the nets of the rule's roles, the plane net from the rule's `plane_net`, by role or by name, default GND) and
  from `--stitch-return PATTERN=PLANE_NET`. The distance is `--stitch-distance`, else the rule's `stitch_via_mm`,
  else the catalogue's 5.08 mm. For a signal via without one, candidate sites on rings around it are tried,
  nearest first (16 directions, ring spacing half a via, positions on whole micrometres); the first site that
  passes is used. The same two-layer condition holds: a return via ties planes together, so a board with the
  plane net on one layer only gets none, and the via is reported as unstitched.

**Which fills.** The fills stored in the file may be older than the routing. The pass refills the zones in the
engine for the routed board (§36) and places against those; hatched zones are left out (the refill does not
redraw them).

**What a site must pass.**

1. The router's exact check for a through via of the net's class size (`via_state`, 10 µm clearer than the rules
   ask), against every pad, track, via, hole, board edge, keep-out, mask opening and custom rule. Surface-mount
   pads of any net, the via's own included, count as obstacles: a stitching via is never a via in pad. Copper
   text is in the obstacle model as an estimated box per line, not as KiCad's glyphs, so stitching vias keep
   0.25 mm more than the clearance from those boxes (a grid via beside the text `JTAG_EN` on
   kit-dev-coldfire-xilinx_5213 was 0.139 mm from a glyph where 0.15 mm is required).
2. Other nets' fills. A through via may pass through another net's plane on a layer in between, as every via on a
   multilayer board does, but only through the middle of it: the hole a refill will cut (via radius + the largest
   clearance + 10 µm) plus the zone's minimum width as a web must lie wholly inside the fill, away from every edge
   of it (outline, other knock-outs, thermal gaps). So the hole cannot cut the fill into islands, sever a spoke or
   leave a sliver. A fill the via neither clears completely nor sits in the middle of blocks the site, and so does
   any hatched fill within reach. New vias keep two knock-outs and a web apart, so their holes never merge. The
   web is the largest minimum width of any zone on the board (at least 0.25 mm): the refill prunes copper narrower
   than a zone's minimum width, and with a fixed 0.25 mm web a 1.4 mm grid through a plane with a 0.5 mm minimum
   width left that plane in loose pieces between the holes, which the refill dropped as islands (KiCad: 270 of
   495 mm² gone where the 164 holes account for 130; no DRC item, no unconnected item).
3. After all sites: a second refill with the vias. A via stays only if its own net's fill overlaps it on at least
   two layers (KiCad joins a via to its net's zone solidly, without thermal relief). If a zone of another net has
   more pieces than before, the whole pass is withdrawn. A stitched net's own zone may gain a piece: an island the
   refill used to drop, now tied to its net by a via.
4. The job's transaction: unconnected items per net after a refill, with and without the vias.

The vias are written with `(free yes)`, so KiCad keeps their net although no track touches them.

**Report.** `stitch` in the JSON summary and log lines: vias per net and purpose, grid sites (in overlapping
fills, already tied, blocked), vias dropped by the second refill, and per signal net its vias and how many have a
plane via within the distance before and after.

**The `reference_plane` rule.** `crules::evaluate` still reports rules of this kind as not applied (the
no-split-crossing part needs plane-coverage checks, doc 15 P4). The stitching part is satisfied and measured here,
per rule instance, as `return_paths` rows with `source` naming the rule and its anchor; it is not fed back into the
component-rule report.

**Limits.** Through vias only. The containment tests walk every edge of the fill a site lies in (no index), which
is the pass's main cost on boards with large fills. Fill outlines contain the zero-width bridges that join holes
to the outline (§36), and a site on such a bridge is refused although the copper is solid there. Signal vias in
dense areas often have no legal site nearby and stay unstitched. Return-path sites are not chosen for the
smallest loop beyond "nearest ring first". The written board keeps its old fills, as with `--soft-zones`: refill
before fabrication output. Every via through another net's plane takes its knock-out out of that plane: at a
tight pitch that adds up (a 1 mm grid took 11 % of the +3.3 V plane of kit-dev-coldfire-xilinx_5213), and nothing
limits it but the pitch. A stitching via beside a teardrop of another net is legal against the pad and the track;
KiCad's refill then cuts the teardrop back around the via (measured on a generated board: no DRC item, the
teardrop's fill 6 to 12 % smaller).

**Status of return-path stitching.** The explicit form (`--stitch-return`) is tested like plane stitching. The
form driven by `reference_plane` rules shares all of its placement code, but on real boards it has placed one via
so far (see §39.4): treat it as experimental until a board set with unstitched high-speed vias exists.

### 39.4 Gates and tests

Measured on 2026-10-11 with `bench/passes_gate.py`: every board is routed twice at the same work budget
(`--variants 1 --no-gpu`), without and with the pass options, and `kicad-cli pcb drc --severity-all
--all-track-errors` (KiCad 10.0.6) judges both. A board passes when the second report has no additional
violation of any type or severity, no additional unconnected item, and the same connections routed.

**Widening.** PCBench boards have no project file; their widths come from the net classes stored in the KiCad 5
board. Boards with necked power nets were found by routing 220 PCBench boards (drawn at random with seed 3 from
those under 400 kB) with `--widen-power` and keeping those where a net with a supply-like name had tracks below
its class width: 16 boards.

| Run | Boards | Pass options | Boards widened | Tracks | Length | Added violations | Unconnected |
|---|--:|---|--:|--:|--:|--:|---|
| `widen-necked` (those 16, 1 M work) | 16 | `--widen-power` | 16 | 144 | 231.7 mm | 0 | equal on all |
| `widen-quick` (`quick` set, 1 M work) | 30 | `--widen-power` and a 0.5 mm target for `GND*`, `+*`, `VCC*`, `VDD*`, `VBUS*`, `VIN*` | 26 | 2,023 | 5,699.6 mm | 0 | equal on all |
| `widen-planes` (`planes` set without RoyalBlue, 3 M work, `--refill`) | 7 | the same | 6 | 274 | 415.3 mm | 0 | equal on all |

The four `quick` boards and the one `planes` board without a widened track have no new track of a matching net
below its target that had room.

**Stitching.** PCBench fixtures carry no zones, so the boards are KiCad demo projects with a net poured on two or
more layers, routing stripped (`bench/prepare_demos.py`), 3 M work, judged with `--refill`. `via_dangling` is
KiCad's item for a via that is not connected or connected on one layer only.

| Board | Layers | `--stitch-vias planes`: vias added | Sites: in fills / tied / blocked | Added violations | Unconnected | `via_dangling` |
|---|--:|--:|---|--:|---|---|
| StickHub | 2 | 1 | 7 / 3 / 3 | 0 | 29 → 29 | 0 → 0 |
| multichannel_mixer | 2 | 210 | 212 / 1 / 1 | 0 | 10 → 10 | 0 → 0 |
| RoyalBlue54L-Feather | 8 | 12 | 61 / 4 / 45 | 0 | 146 → 146 | 0 → 0 |
| kit-dev-coldfire-xilinx_5213 | 4 | 106 | 257 / 14 / 137 | 0 | 451 → 451 | 0 → 0 |
| tinytapeout-demo | 4 | 137 | 255 / 93 / 25 | 0 | 214 → 214 | 0 → 0 |
| CM5_MINIMA_3 | 6 | 19 | 83 / 13 / 51 | 0 | 123 → 123 | 0 → 0 |
| One-Air-Max | 4 | 93 | 215 / 22 / 100 | 0 (one `isolated_copper` warning fewer) | 147 → 147 | 0 → 0 |
| complex_hierarchy | 2 | 0 | 0 / 0 / 0 | 0 | 3 → 3 | 0 → 0 |

Seven boards got vias; no via was dropped by the second refill. complex_hierarchy's two-layer pours are too small
to hold a 5.08 mm grid site. (The router completes few connections on the large boards at this budget and without
`--soft-zones`; the stitching is judged on whatever was routed.)

Return-path stitching, the same boards and budget, `--stitch-vias return --stitch-return '*=GND'` (every net as a
signal, to have vias to stitch), distance 5.08 mm:

| Board | Signal vias | Unstitched before → after | Vias added | Added violations | Unconnected | `via_dangling` |
|---|--:|---|--:|--:|---|---|
| StickHub | 12 | 1 → 0 | 1 | 0 | 29 → 29 | 0 → 0 |
| multichannel_mixer | 9 | 9 → 4 | 5 | 0 | 10 → 10 | 0 → 0 |
| tinytapeout-demo | 18 | 1 → 0 | 1 | 0 | 214 → 214 | 0 → 0 |
| One-Air-Max | 46 | 34 → 19 | 11 | 0 | 147 → 147 | 0 → 0 |
| kit-dev-coldfire-xilinx_5213, CM5_MINIMA_3 | 0 | – | 0 | 0 | equal | 0 → 0, 0 → 0 |

Driven by the `reference_plane` rule (`--component-rules report --stitch-vias return`): on the stripped boards the
router brings the USB pairs in without vias, so there is nothing to stitch. On the designers' own routings (the
unstripped demo boards, where the pass only adds vias) USB2-09 binds D+/D- on StickHub, CM5_MINIMA_3,
RoyalBlue54L-Feather and One-Air-Max; at the catalogue's 5.08 mm every bound via already has a GND via in reach
except one on One-Air-Max (`/USB_D+`), which the pass stitches (1 → 0 unstitched, KiCad: nothing added). At 3 mm
and 1.5 mm the unstitched vias on StickHub (2 at 1.5 mm), CM5_MINIMA_3 (4 at 1.5 mm, 1 at 3 mm) and One-Air-Max
(2) have no legal site that close and stay unstitched; KiCad's reports are unchanged. That one via is all the
evidence there is for the rule-driven path on real boards.

**Second sample (review of the passes, same day, same judge; unconnected counts and violation counts per type
and severity compared).**

- Widening: 48 other PCBench boards (drawn with another seed from those under 500 kB, none from `quick` or the
  16 above), 1 M work, targets of 0.5 to 0.8 mm for supply-like names: 40 boards widened, 3,466 tracks,
  5,852.2 mm; nothing added, unconnected equal on all.
- Widening under custom rules: ten `quick` boards with a generated `.kicad_dru` (a wider clearance for tracks of
  0.4 mm and more, `track_width (max 0.45mm)` on B.Cu, `disallow track` above 0.7 mm), 0.8 mm targets: 865
  tracks widened, nothing added. Before `RuleEngine::track_width` returned a rule's maximum, the same run added
  96 `track_width` errors on eight of the boards.
- Plane stitching on the designers' own routing (the unstripped demo boards, where the pass only adds vias), at
  2 mm and 1 mm pitch: kit-dev-coldfire-xilinx_5213 251 and 491 vias, One-Air-Max 106 and 276, tinytapeout-demo
  229 and 465, CM5_MINIMA_3 6 and 12, RoyalBlue54L-Feather 6 and 7; nothing added, unconnected equal. Fill
  pieces per net and layer after KiCad's refill change only for the stitched nets (islands now tied), and other
  nets' planes lose the holes only (kit-dev at 1 mm: +3.3 V on In2.Cu, one piece before and after, 10,757.8 to
  9,536.1 mm²).
- The two largest demo boards, with their own `.kicad_dru`, designers' routing: jetson-agx-thor-baseboard
  (10 layers) 8 vias at 5.08 mm and 34 at 2 mm, vme-wren (12 layers, one hatched zone) 16 and 194; nothing added
  (222 and 446 items before and after), unconnected 0 → 0. KiCad's `length_out_of_range` figures on vme-wren
  differ by some hundredths of a millimetre between two runs on the same file, so those items are compared by
  count.
- Return-path stitching with `--stitch-return '*=GND' --stitch-distance 1.5` on the same designers' boards and
  StickHub: 103 vias added over six boards (79 on tinytapeout-demo, 19 on One-Air-Max), nothing added.
  `--component-rules report --stitch-vias return --stitch-distance 2`: six bound vias without a GND via in
  reach (One-Air-Max 2, CM5_MINIMA_3 4), none with a legal site, no via added.
- Stripped boards routed with `--soft-zones` at 3 M work, then all passes together (2 mm pitch, `'*=GND'`,
  0.5 mm targets), six boards: nothing added, unconnected equal.

**Shared.** `quick`, 1 M work, `--variants 1 --no-gpu`, no new option: all 30 routed boards byte-identical to the
baseline binary of unmodified main (`compare_runs.py --expect-identical`, the baseline run made before the style
metric existed compares without errors); `summary.json` carries `style`, `style_ref` and `style_same_boards`.

Tests: `tests/test_passes.cpp` (21 cases: each style pattern on hand-made polylines, order independence; a
channel that fits 0.8 mm but not 1.2 mm, a track with no room, a junction, a via limit, locked tracks, pairs,
neck-down restored, the work budget, a custom rule's maximum width; the expected grid in a two-layer overlap, an
obstacle and a keep-out removing exactly their sites, a site inside another net's clearance, a via through the
middle of another net's plane but not its edge, holes through a plane with a wide minimum width, the margin from
copper text, nothing without overlapping fills, return-path vias) and the ctest `after_route_passes`
(`tests/integration/after_route_passes.py`: the three passes through the command line on generated boards, style
equal at 1 and 3 threads and equal to `tracemaker style`, KiCad's verdict on the stitched board when kicad-cli is
there).

## 40. Dense breakouts: limits, portfolio extras and the experiments that did not help (M14, 2026-10-11, D94, D95)

Measured on tscircuit's BGA breakout sets (doc 10 §8, doc 16 §10); this section records what the router now does and what the
hidden options are.

- **Defaults.** `max_rips_per_connection` 40 and `max_passes` 40 (were 8 and 12): negotiation on a dense breakout ends because a
  connection reached its rip limit, not because the budget ended. 33 % → 54 % clean on the srj19 gap boards at 50 M work.
- **Budget accounting.** `--work` counts search expansions *and* reachability flood cells (1:1). On a 60-connection two-layer
  breakout at 10 M the strict first pass and its floods use all of it (9.5 M of 10 M), so negotiation never runs; read a result at
  10 M as "first pass only".
- **Portfolio beyond eight.** `--variants N` (N > 8, up to 64) adds seeded variants: fast bends, jittered order (every fourth
  shortest-first), via cost × {0.4, 1, 2.5}, crossing cost × {0.5, 1, 2}, escape plan on even indices, `--package-pen 3` on odd ones.
- **`--package-pen P`.** Inside the pad box of a footprint with at least 30 SMD pads on one layer, tracks on that layer cost P
  pitches more per lattice step, so an escape takes a dog-bone via early. srj20 gap, 30 M: 13 → 17 clean of 57 at P = 4, 10 at 8.
- **Hidden experiment options** (results in doc 16 §10; none helps): `--guide FILE`, `--guide-pen`, `--guide-radius` (per-connection
  preferred paths; a hard search inside the guide first, cut off at a few expansions per cell, then the usual search with a mild
  preference), `--layer-assign`, `--order 3`, `--lns-open N`, `--no-strict-first`, `--max-victims`, `--soft-growth`,
  `--history-weight`, `--max-rips`, `--max-passes`, `--max-restarts`, `--soft-cost-mm`.
- **GPU.** The cost-to-go fields give the identical result 1.7× faster for one board alone (35.7 s → 20.7 s, sample 13 at 20 M); with
  twelve boards routed in parallel they are slower (contention), so the benchmark runs use `--no-gpu`.
- **`--layer-dir-pen P`, `--layer-dir-swap`** (D98). Orthogonal steps against the layer's preferred direction (even layers horizontal, odd
  vertical) cost P pitches; diagonals are free. Automatic (P = 0.3, with `--package-pen 3`) on a breakout board: two copper layers and at least 70 %
  of the connections with an end on a ball-grid-like package (at least 30 same-layer SMD pads on at least 5 × 5 distinct positions, 40 % of the grid filled). Negative values mean "automatic at that strength" (the seeded
  portfolio variants use −0.3, −0.6, −1.01). Elsewhere the penalties cost completion (quick 89.2 → 86.8 %), so they stay off.
- **Portfolio extras** (variants 9 and up) now vary the direction strength {0.3, 0.6, 1.0} and swap the directions on every fifth.
- **More hidden options, measured and not useful:** `--polish F` (reserve a budget share for rebuilding the open connections of the best state),
  `--layer-assign-angle`, `--heuristic-weight` above 1 (weighted A*: 4 % clean at 1.5), `--lns-open`.
