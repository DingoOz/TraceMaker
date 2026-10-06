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
| 1 | Clearance (all object pairs incl. copper text/graphics), track width, via size/drill/annular ring, hole-to-hole, hole clearance, edge clearance, keepouts and rule areas, net-class rules, `.kicad_dru` clearance/width conditions on net class, layer and area; `disallow` and `physical_hole_clearance` (§16) |
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
| GPU cost-to-go fields as the heuristic (never used to prune) | Done | `gpu/field_cuda.cu`, `build_field` |
| Portfolio of 8 variants on a thread pool, early stop when one is complete (wall-clock mode), 2x pitch for two variants on large boards; with `--work` all 8 run at any `--threads` and the winner is chosen by a total order ending in the variant index, so the output is bit-identical across thread counts (D47) | Done | `route_portfolio` |
| Global routing, first CPU version: tile graph (8 pitches), exact edge capacities, negotiated congestion, soft corridors (`--global`, off by default) | Experimental: no gain yet. On AmpOne, USBI2C01 and motor-3xdrv8833 (60 s, one variant) corridors shortened track a little but did not raise completion and sometimes added vias. Missing: Steiner topology, via capacity, layer assignment without via columns, corridor-restricted windows | `route/global_router.cpp` |
| Clean-up (section 8): via-saving re-routes, region rip-up around vias, path smoothing | Done | `optimize_vias`, `lns_vias`, `smooth_paths` |
| Escape planning (section 3), version 1 (M9, 2026-10-04) | Partly done: escape corridors (opt-in), feasibility analysis, via neck-down, dead pins. Version 2 (2026-10-05): min-cost-flow channel and layer assignment for deep arrays, opt-in (`--escape-flow`), measured below version 1. Not built: NC fallback, escape templates. See §12, §14 | `route/escape.{hpp,cpp}`, `router.cpp` |
| Differential pairs (version 2, M12): coupled pair search with coupled vias, breakout/fan-in legs, re-coupling after rip-up, enclosed-pin check; length tuning (custom `length` rules) and skew tuning (custom `skew` rules, `--pair-skew-mm`) | Done, opt-in (`--diff-pairs`, D50). Not built: pair twists, pairs ending on routed copper, pair-aware global routing. See §15 | `route/router.cpp` (`route_pair`, `tune_skew`), `route/diff_pair.{hpp,cpp}` |

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
| Coupled search (`route_pair`) | A* over (layer, lattice point, direction, which half is on the left). Moves: one straight lattice step; a 45° turn followed by K straight steps, K·pitch ≥ 2·offset·tan 22.5° + width so the inner track's miter never folds back; a coupled via pair (both halves jog out at 45° to the via spacing, change layer side by side, jog back; MV steps). Each move is checked exactly (`segment_state`, `via_state` against fixed and routed copper) on both offset tracks, so the coupled section keeps its gap by construction. Cost: length + K·pitch per turn + two vias; heuristic 2 × straight-line distance to the end pads plus a via pair while off the end pads' layers (weighted: legal, not optimal). Budget 600 states per lattice step of the pair's length (40 k–250 k), counted as work |
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

**Not built.** Pairs whose pin order is mirrored between the ends need a twist (one half crosses the other through a
via): such pairs (ULPI-Pmod, android_debug_cable) are routed singly. A pair ends on pads only, so a pair joining copper
already routed for its nets (a T at an AC-coupling capacitor or termination) often fails its goal legs. No pair-aware
global routing; no rounded or arc corners; skew in picoseconds needs the stackup (doc 15 §5.3); tuning meanders on
one half reduce coupling locally (both halves meandering together is not built); per-pair skew limits from the
component-rule catalogue are not wired (one global `--pair-skew-mm`).

## 16. Custom-rule routing (2026-10-06, D56)

**Before.** Any custom rule disabled the obstacle cache and cost-to-go fields. Via-only keepouts were ignored.

**What was built**

| Rule | Router | DRC (`tracemaker drc`) |
|---|---|---|
| `disallow track` by net, net class, type or layer, including `inDiffPair` | `RuleEngine::track_allowed` supplies per-net layer masks for pad cells, escapes, planar moves, via landings, diff-pair legs, escape corridors and fields. Through vias may pass through disallowed track layers. | `items_not_allowed`, once per item |
| `disallow via`, `through_via`, `micro_via`, `buried_via` or `blind_via` with those conditions | `via_allowed` rejects vias for a net if a through via matches on any layer; the net then gets no blind or buried vias either (`--blind-vias`). | `items_not_allowed` |
| Positional, footprint or pad-dependent `disallow` (`insideArea`, `intersectsArea`, `enclosedByArea`, `memberOfFootprint`, `Reference`, `Pad_Type`, `Width`) | Warned; not applied. | Reported |
| `disallow hole / footprint / text` | Warned; not applied. | Left to KiCad |
| `physical_hole_clearance` | `Obstacles::physical_hole_blocked` checks new via holes against fixed copper of any net, including the same net, on cached and exact paths. Same-net routed copper is not checked. | `hole_clearance`, once per hole and item, any net |
| Keepout rule areas | Tracks and vias use their respective keepout flags. | Unchanged |

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

## 17. Search speed on fine lattices (2026-10-06, D57)

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
expansions by 0.01% without changing output.

**Second round** (2026-10-06). On the same 20M check, `/usr/bin/time -l` instructions retired repeated within
0.2%; wall and user time varied ±20% with shared machine load. Runs used `--threads 2`; work-budget output is
thread-count independent. Peak memory is ~0.4–0.8 GB per concurrent variant (2.8 GB at 2 threads, 6.3 GB at 8).

| Part | Change | Effect |
|---|---|---|
| `Obstacles::fixed_via_code` | One copper query across layers; check holes, edges, keepouts and mask openings once. Grid tests compare with `fixed_via_code_reference`. | user CPU −7% |
| `Shape::set_point`, `closer_than_disk` | Reuse scratch disks and test routed via holes without shape allocation. | user CPU −10% |
| `seg_seg_closer` | Reduce a degenerate segment to one point-to-segment test; exact parity with the general four-way test. | instructions −3.5% |

Outputs remain byte-identical at 5M and 20M on the private board; quick tier 30/30.

**Public boards** (`bench/speed_ab.py`, Apple M4 Pro): the engine before and after D56–D57 on the same boards,
one variant, one thread, CPU fields, fixed work. Instructions retired from `/usr/bin/time -l`.

| Set | Work | CPU s before → after | Instructions | Speed-up | Output |
|---|--:|--:|--:|--:|---|
| 10 large PCBench boards (`bench/m6_bench.py` set; no project files, default rules) | 30M | 100.7 → 83.2 | 1,511 G → 1,221 G | 1.21× (1.09–1.32×) | 10/10 identical |
| 6 small KiCad demo projects (`--demos`; net classes with name patterns) | 2M | 184.1 → 75.8 | 5,023 G → 2,019 G | 2.43× (2.15–2.73×) | 6/6 identical |

The gain is largest where net classes use name patterns, which the old per-cell lookup matched every time.
The two largest demos (vme-wren, jetson-agx-thor) route nothing within 1M work and take 128 s and 60 s doing so
(jetson 2.1× faster after, vme-wren unchanged); where that time goes is not yet profiled.

**Reverted.** A 4-ary open heap and pre-rule bounding-box filters gave no measurable gain. Combining routed
queries was slower; per-layer/via-only routed indexes gave −2% within noise and added five indexes; a combined
physical-hole query gave −0.3%.

**Remaining costs.** Open list ~16%, routed-copper queries ~17%, fixed-copper via checks ~16% (half rule
evaluation), fields ~10%. Fixed-obstacle caches remain per variant: up to eight copies of the same lattice
codes; sharing them needs a thread-safe cache.
