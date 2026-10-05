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
| 1 | Clearance (all object pairs incl. copper text/graphics), track width, via size/drill/annular ring, hole-to-hole, hole clearance, edge clearance, keepouts and rule areas, net-class rules, `.kicad_dru` clearance/width conditions on net class, layer and area |
| 2 | Zone connections (thermal reliefs to planes), solder-mask bridge awareness, blind/buried/micro vias |
| 3 | Differential pairs (coupled routing as a single wide "pair" object in search), length and skew targets (meander tuning in cleanup), max uncoupled length |

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
| Escape planning (section 3), version 1 (M9, 2026-10-04) | Partly done: escape corridors (opt-in), feasibility analysis, via neck-down, dead pins. Not built: min-cost-flow channel assignment, layer assignment, escape templates. See §12 | `route/escape.{hpp,cpp}`, `router.cpp` |
| Diff pairs and length tuning | Not started | |

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

**Not built yet (rest of M9).** Min-cost-flow channel assignment for arrays deeper than two rings (Yan & Wong),
layer assignment per ring, escape templates in the knowledge base (doc 06 T3), and completion over escapable
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

