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
| Portfolio of 8 variants in threads, early stop when one is complete, 2x pitch for two variants on large boards | Done | `route_portfolio` |
| Escape planning (section 3), global routing (section 4), cleanup (section 8) | Not started | |
| Diff pairs and length tuning | Not started | |
