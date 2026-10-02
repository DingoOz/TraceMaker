# A Clean-Sheet PCB Autorouter in C++ — Design Document

> **Status:** design proposal, not implemented.
> **Date:** 2026-10-02
> **Sources:** the literature review in `robust_autorouting_research.md` (same folder), the source code
> of existing routers, and what was measured while improving Freerouting on branches
> `feature/grid-fallback-router` and `feature/ncr-pcb`.

---

## 0. Summary

A new router should be built as a **pipeline of stages around one transactional board model**, not as one
clever search:

1. **Ingest the full design rules from the EDA tool**, not a lossy interchange format.
2. **Analyse routability and plan escapes** for dense parts.
3. **Global routing** on a coarse 3-D graph with negotiated congestion.
4. **Gridless detailed routing** inside the global corridors.
5. **DRC-marker-driven negotiated repair.**
6. **A last-gasp stage** for the final few connections.
7. **Cleanup and DRC sign-off** with the same rules the EDA tool uses.

Every stage has a time budget derived from the job's remaining time, and works in transactions that are
kept only if a measured score improves. The router is judged by the EDA tool's own DRC ("clean pass":
fully connected and zero errors), not only by its own checker.

The single most important lesson from the Freerouting work is that **routing completion is only half of
the problem**. On the 109 PCBWorld test boards, Freerouting completed 98 but only 33 passed KiCad's DRC.
The causes were all rules the interchange format did not carry, or that the router ignored:

| KiCad DRC error | Errors | Boards |
|---|--:|--:|
| `track_width` (neckdowns below the board minimum) | 339 | 55 |
| `copper_edge_clearance` | 236 | 41 |
| `solder_mask_bridge` | 93 | 8 |
| `clearance` and `shorting_items` (tracks through copper text) | 8 | 4 |

---

## 1. Goals and non-goals

**Goals**
- **Clean pass first.** Full connectivity with zero EDA-tool DRC errors on routine 2–8 layer boards, then
  completion on dense boards (BGA, fine-pitch QFN/QFP).
- **Speed.** Interactive-scale run times: seconds for small boards, minutes for dense ones.
- **Anytime behaviour.** A best-so-far board is always available; more time only improves it.
- **Determinism.** Same input, settings and seed → same output, including with many threads.
- **Embeddable.** A C++ library with a CLI, a KiCad IPC plugin and Python bindings.

**Non-goals for version 1**
- Placement. The router *reports* placement problems (§5.1) but never moves parts.
- Full high-speed sign-off (impedance, crosstalk). Hooks exist for length and differential-pair rules.
- Any-angle or arc routing. Octilinear (0/45/90°) first; arcs are a later cleanup pass.
- A topological (rubber-band) core. Every open-source attempt is self-described as experimental (gEDA
  toporouter, pcb-rnd `rt_topo`, Salewski), and the commercial ones (Altium Situs, TopoR) still finish
  with geometric passes.

---

## 2. What the evidence says

### 2.1 From the literature

| Lesson | Source | Evidence |
|---|---|---|
| Negotiated congestion: cost = (base + history) × present congestion; reroute repeatedly | PathFinder (FPGA 1995), VPR | Routes circuits sequential rip-up cannot; VPR defaults `pres_fac` 0.5 × 1.3 per iteration, `acc_fac` 1 |
| Keep base cost separate; exponential congestion; a "last gasp" free-space mode | FGR (TCAD 2008) | Over 75% of iterations were spent at under 0.01% overflow before last gasp |
| Scale present cost so history cannot outgrow it; decay history | BoxRouter 2.0, FastRoute, CUGR | Without scaling, overflow climbs again |
| Reroute only illegal sub-trees; grow the search box on contact | AIR (ASP-DAC 2020) | 7× faster than VPR 7 |
| Two stages: global guides become a soft cost bias in the detailed router | FastRoute → TritonRoute, Dr.CU | Industry-standard handover |
| DRC-marker-driven repair with an aggressor/victim queue, shifted windows, reroute caps | TritonRoute-WXL (TCAD 2021) | Runtime −33.5%; every testcase converged |
| Trial ordering of the last nets: commit the one that traps the fewest others | B-Escape (ISPD 2010) | 14/14 boards vs 7/14 for Allegro |
| Routability = every gap between obstacles fits the wires through it | Maley; Yu & Dai (1997) | Gridless, incremental, names the over-full gap |
| Tile-based gridless search; bidirectional; several paths per tile | Contour (DEC WRL 1995) | Bidirectional search exposes a boxed-in terminal |
| Escape routing as network flow with correct diagonal capacity | Yan & Wong (DAC 2009) | First optimal escape model |
| Complete PCB flow: escape (layer + order), refinement, area routing | Lin et al. (DAC 2021) | 7 commercial boards a commercial tool could not finish |
| Learned fanout points help dense boards | FanoutNet (AAAI 2023) | 100% on bm1–bm11 where Freerouting 1.4 reached 93–99% in > 24 h |
| Best-of-N seeds lifts clean pass | PCBWorld (2026) | Its baselines are reported as best of 5 rollouts |

### 2.2 From the Freerouting experiments (this repository)

| Finding | Number | Consequence for the new design |
|---|---|---|
| Failures concentrate in the last 1–9 connections | 30 of 103 gap boards | Dedicated last-gasp stage (§5.6) |
| A grid router with rip-up and transactional acceptance closes many of them | 11/33 boards better, 0 worse; unrouted nets −33% | Keep the transactional "keep only if better" pattern everywhere |
| A plain grid search without rip-up helps little | 14 of 88 nets | Rip-up and negotiation are the lever, not the grid |
| KiCad DRC fails boards Freerouting's own DRC calls clean | 33/99 clean pass on D3-A | Ingest full rules; DRC sign-off with the tool's rule set (§4, §5.8) |
| A "no new violations" check on new items alone misses one-sided pairs | +9 violations on 5 boards | Check both sides of every pair touching new geometry |
| Library calls ignore their own time limits on degenerate geometry | one pull-tight call took > 20 s | Budgets enforced by the scheduler, not by callees (§6.3) |
| The batch router stops itself through the same flag a user stop uses | grid stage silently skipped | Explicit stage results, never shared stop flags |
| Routing is not deterministic under CPU load | different results flag-off vs earlier run | Deterministic scheduling and seeds from day one (§6.2) |

---

## 3. Architecture

```
            ┌──────────────────────── Job (budget, seed, settings) ─────────────────────────┐
 Input ─▶ Ingest ─▶ Rules compile ─▶ Analysis ─▶ Escape plan ─▶ Global NCR ─▶ Detailed ─▶ Repair ─▶ Last gasp ─▶ Cleanup ─▶ DRC sign-off ─▶ Output
            │            │                           ▲               │               │          │                         │
            │            │                           └──── congestion history (shared, decaying) ◀───────────────────────┘
            └──── Board model: layered geometry + spatial index + transactions (branch / commit / rollback) ────┘
```

Each stage:
- reads and writes the **board model** only through **transactions** (§4.4);
- gets a **budget** (wall time and work units) from the job scheduler and must return by it;
- returns a **stage report** (counts, timings, warnings);
- is **replaceable** (same interface), so variants can race in a portfolio (§6.4).

---

## 4. Core data model

### 4.1 Geometry kernel

- **Coordinates:** 64-bit integers in nanometres. Avoids the resolution-dependent rounding seen in DSN
  imports.
- **Primitive shapes:** point, segment with width (stadium), octilinear polygon, circle (padstack),
  arbitrary polygon (zones, outlines).
- **Clearance tests:** exact integer predicates for orientation and intersection (no epsilons), and
  octilinear "bloat by clearance" operations.
- **Polygon booleans:** [Clipper2](https://github.com/AngusJohnson/Clipper2) (Boost licence) for zones,
  mask apertures and outline offsets.
- **Triangulation**, only if the routability check of §5.1 needs it: [CDT](https://github.com/artem-ogre/CDT)
  (MPL-2.0). Avoid CGAL's licence and build weight unless needed.

### 4.2 Layered board model

- **Layers:** copper (signal or plane, active or inactive), drill spans, mask, paste, edge cuts and courtyards.
- **Objects:** pads (padstack per layer), vias (types and spans), tracks, zones (filled and unfilled),
  keepouts (per-layer type flags: tracks, vias, pads, pours), copper graphics and **copper text**
  (obstacles — missing them caused real shorts in the KiCad test above), board edge and cutouts.
- **Nets and net classes:** a net class gives width, clearance, via type, and length and differential-pair
  rules. Ownership and fixed state are tracked per object; nothing the user fixed is ever ripped.

### 4.3 Spatial index

- One **R-tree per layer per object family** (Boost.Geometry `rtree` with quadratic or R* split, or a packed
  Hilbert R-tree rebuilt per stage), storing *clearance-bloated* bounding boxes per clearance class.
- The detailed search uses a **tile plane** per layer and clearance class: corner-stitched clear and solid
  tiles (Contour). The router searches zero-width centrelines, because obstacles are pre-bloated by
  `(width + 2·clearance)/2`. Tiles are rebuilt incrementally around changed areas.

### 4.4 Transactions (branch / commit / rollback)

The pattern that made the grid fallback safe, built in from the start:
- `Txn t = board.branch();` — a copy-on-write overlay over the parent state (as in KiCad PNS's
  `NODE::Branch`).
- Stages add and remove objects in the branch. The overlay tracks the **new and removed ids** and the
  **dirty region**.
- `t.score()` = (incomplete nets, incomplete connections, DRC errors added, length, vias), computed
  **incrementally** over the dirty region plus a cached global part.
- `t.commit()` only if the score is lexicographically better (or equal and allowed); else `t.rollback()`,
  which is O(changes).
- Branches nest. A repair window can try several orderings in sibling branches and keep the best.

This removes the most expensive Freerouting pattern: undo stacks plus full-board DRC recounts per attempt.

### 4.5 Congestion maps

- **Global grid:** a 3-D tile graph sized at roughly 8–10 track pitches per tile, clamped to at most about
  256 tiles per side. Each tile holds capacity (tracks across each edge, via sites) and demand.
- **History `h`** per tile and layer: increments on overflow, rip-ups and failures; decays ×0.9 per
  iteration.
- It is shared by every stage. Global routing writes it; detailed routing and repair read and update it.

---

## 5. Stages

### 5.1 Ingest, rule compilation and analysis

**Ingest**
- Read **`.kicad_pcb` and `.kicad_pro` directly**, or use the KiCad IPC API, so every rule is available.
- Specctra DSN/SES is supported only as a legacy path. **Never route on rules the importer could not
  read**: warn and use conservative defaults.

**Rules compile.** One query object answers `clearance(a, b, layer)`, `minWidth(net, layer)`,
`edgeClearance`, `holeToHole`, `maskExpansion(pad)` and `viaRule(net)`. Each rule is resolved once into
per-class tables. A subset of KiCad custom DRC rules (conditions on net class, layer, area) is compiled
to predicates.

**Analysis report** (also user-facing placement feedback):
- RUDY wiring demand per tile versus available copper, with hot spots listed in mm.
- Airline crossings per layer pair: a lower bound on vias or detours.
- Pin-escape capacity per dense component, from Maley cuts between adjacent pads. Pins that cannot escape
  on any layer are reported as **dead** and excluded from routing.
- **Net ordering:** dead connections removed, then hardest first by demand ÷ capacity along the bounding
  box, then shortest.

### 5.2 Escape (fanout) planning for dense parts

- For BGAs and fine-pitch arrays, decide each pin's **escape direction, layer and via site** before area
  routing, using min-cost flow on the pin-grid escape graph with correct diagonal capacity (Yan & Wong).
- Order and layers come from the simultaneous-escape idea of Ozdal & Wong and Lin et al.: a maximal
  planar subset per layer, assigned greedily.
- A negotiated-congestion fallback on the escape graph (Ma, Yan & Wong) covers what the flow model leaves
  unrouted.
- **Output:** a fixed-but-rippable escape stub per pin and a target boundary port for global routing.

### 5.3 Global routing (negotiated congestion)

- **Unit:** two-pin connections from a rectilinear Steiner tree per net, rebuilt when pins merge. Plane
  nets become via drops to the plane.
- **Search:** A* on the tile graph.
  - The heuristic is octile distance plus `minLayerChanges × viaCost`: a via-aware lookahead table
    precomputed per layer pair (VPR's map lookahead idea).
  - The search box is the bounding box ×3, grown when the route touches its edge (AIR).
- **Edge cost** (FGR form with BoxRouter scaling):

  ```
  c(e) = b(e) + α · h(e) · p(e)
  p(e) = 1 + pres_fac · max(0, use(e) + w − cap(e)) / cap(e)     pres_fac = 0.5 · 1.3^k, capped
  h(e) ← 0.9 · h(e) + overflow(e)                                each iteration
  α    = max_e h / p_full                                        keeps present overflow dominant
  ```

- **Iterations:** reroute only connections through over-used edges (AIR's incremental rip-up), and keep
  the best-overflow solution (FastRoute).
- **Last-gasp switch:** under about 1% overflow, route through free capacity only, at base cost (FGR).
- **Output:** per connection, a **corridor** (tiles per layer) and preferred via tiles; per tile, the
  remaining overflow, which is written into `h`.

### 5.4 Detailed routing (gridless, in corridors)

- **Search:** A* over the tile plane, bidirectional (Contour), with **several non-dominated labels per tile**
  (cost-cone pruning). Freerouting's one-label-per-door rule can lose a cheaper path that arrives later.
- **Cost:**
  - length plus bends plus vias;
  - plus `h`-weighted congestion: `(1 + w_h · h(tile))` on each step;
  - plus a **soft corridor penalty** (out-of-corridor steps cost more; the penalty fades after the first
    iteration, as in TritonRoute);
  - plus a present-conflict cost for passing through rippable foreign copper.
- **Widths:** the net-class width. **Neckdowns never go below the narrowest width the design declares, nor
  below the board minimum.** Freerouting's "micro neckdown" to 3/4, 3/5 and 1/2 of class width produced
  KiCad `track_width` errors on half the test boards.
- **Insertion** happens in a transaction. A cheap local pull-tight runs with a hard **work** budget, not
  only a wall-clock check; cleanup proper happens later (§5.7).

### 5.5 Negotiated repair (replaces pass-wide rip-up)

- Find DRC markers and failed connections.
- **Aggressor/victim queue** (TritonRoute): for each marker, the earlier-routed nets in it are queued for
  reroute and the last-routed net only for a re-check. Each net has a reroute counter, capped at about 8;
  at the cap it goes to the last-gasp stage.
- **Clip windows** around markers, shifted between iterations so window borders move; the window grows
  while progress stalls.
- Marker cost and `h` updates feed back into global routing. Every K rounds, global routing reruns
  incrementally so corridors move away from contested tiles.
- **Stopping:** zero markers; or a failure predictor (VPR) says the trend cannot reach zero in budget;
  or the budget runs out. The best board is always kept.

### 5.6 Last gasp (the final few connections)

Proven in the Freerouting grid fallback; generalised here.
1. **Strict free-space routing** of each remaining connection, from both ends, at two grid pitches.
2. **Rip-up search:** cross foreign unfixed copper at a cost (10 grid steps × history). Rip the crossed
   connections, route the target (gridless first, the grid path otherwise), then reroute the victims
   (gridless, grid, then grid with rip-up two levels deep, protecting already-placed nets).
   **Negotiation:** when a victim cannot be restored, raise its crossing cost and search again (up to 4
   rounds; stop when the rip set repeats).
3. **Repair windows:** rip everything foreign around the connection (bounding box + 4, then + 12 track
   pitches) and reroute the contents under several orders, each in a sibling branch.
4. **Trial ordering** (B-Escape): tentatively route each remaining connection, count the others it traps
   or blocks, and commit the least damaging.
5. **Cut-capacity diagnosis** (Maley): when a cut along the corridor is over-full on every layer, report
   the connection as **placement-limited** and stop retrying it.

**Acceptance for every step:** strictly fewer incomplete nets (or, at equal nets, fewer connections) **and
no new DRC errors**. "No new errors" is checked on both sides of every pair touching new geometry, then
confirmed by the incremental board score.

### 5.7 Cleanup

- **Pull-tight** (rubber-banding toward the shortest legal octilinear path), **via minimisation**
  (layer reassignment of segments), and **45° smoothing** and corner reduction (KiCad PNS optimiser ideas:
  merge segments, smart pad exits, obtuse corners).
- **Optional any-angle and arc conversion** for appearance.
- Each pass is a transaction under the same acceptance rule, with work budgets.

### 5.8 DRC sign-off

- **The router's DRC implements the EDA tool's checks** on the compiled rules: clearance (copper, hole,
  edge, text and graphics), widths, annular rings, mask bridges, courtyards (report only) and connectivity.
- **Benchmarks also call the tool itself** (for example `kicad-cli pcb drc`) and count *added* errors
  against the unrouted board. Freerouting's own DRC reported clean boards that KiCad rejected; the new
  router must not repeat that.

---

## 6. Execution model

### 6.1 Threading

- **Global routing:** batches of connections whose search boxes do not overlap, routed in parallel
  (Dr.CU-style bulk synchronisation), with deterministic merge order.
- **Detailed routing and repair:** region-partitioned work items (windows) on a thread pool (oneTBB or a
  small work-stealing pool). Windows that overlap are never run at the same time; a deterministic
  scheduler picks the next non-conflicting window.
- **Shared maps** (history, usage) are updated in per-thread buffers and merged at batch barriers. No
  racy shared writes; determinism matters more than the last percent of speed.

### 6.2 Determinism

- One `uint64` seed per job and a counter-based RNG (Philox or SplitMix64) keyed by (seed, stage, item),
  so results do not depend on thread timing.
- Stable tie-breaks everywhere (object ids, then coordinates). Never iterate hash containers in
  output-affecting order.
- Time budgets change *how much* work is done, never *which* choice is made at a decision point. Under
  load a run may stop earlier, but the same work units produce the same board.

### 6.3 Budgets

- The job scheduler hands each stage a deadline plus a work quota (search expansions, DRC checks).
  Callees check the **work quota** in their inner loops. Freerouting's time limits were ignored inside
  trace normalisation, and one call ran over 20 s against a 1 s limit.
- The default split: stages share the remaining time with fixed floors and caps (for example last gasp
  at most 120 s and at most half of what is left), so the cleanup stage always has time.

### 6.4 Portfolio

- Run N variants (seeds, cost schedules, net orders) as independent jobs on board branches, give more
  budget to the leaders (successive halving), and keep the best by the clean-pass score.
- This is the cheapest robustness gain once runs are deterministic; PCBWorld reports its baselines as
  best of 5.

---

## 7. Interfaces

- **Library API** (C++20):

  ```cpp
  Board board = io::readKicad("board.kicad_pcb", "board.kicad_pro");
  RouterSettings settings = RouterSettings::defaults();
  RouteResult result = route(board, settings, Budget{std::chrono::minutes(5)}, /*seed*/ 1);
  io::writeKicad(board, "routed.kicad_pcb");   // or an SES for legacy flows
  ```

- **CLI:** `route board.kicad_pcb -o routed.kicad_pcb --time 5m --seed 1 --report report.json`. Settings
  use lowercase `snake_case` with dots (for example `--router.global.iterations=20`).
- **KiCad plugin:** through the KiCad IPC API — read the board and rules, route out of process, write
  tracks back. Report progress and the analysis report in the UI.
- **Python bindings** (pybind11): for benchmark harnesses and learned-ordering experiments
  (Liao et al. 2026 drive Freerouting's net order with reinforcement learning).
- **Report JSON:** stage reports, the routability analysis, unrouted connections with reasons
  (dead, placement-limited, budget), and DRC summaries.

---

## 8. Code layout and tooling

```
/core        geometry kernel, ids, arena allocation, RNG, budgets
/model       board, layers, nets, rules compiler, transactions
/index       R-trees, tile planes (corner stitching), congestion maps
/analysis    RUDY, crossings, escape capacity, routability report
/escape      flow-based escape planner, NC fallback
/global      tile graph, Steiner decomposition, NCR router, lookahead tables
/detail      gridless A* (multi-label, bidirectional), insertion, neckdown rules
/repair      DRC markers, aggressor/victim queue, windows, last gasp
/cleanup     pull-tight, via minimisation, smoothing
/drc         rule checks equivalent to the EDA tool's
/io          kicad_pcb/kicad_pro (s-expression), KiCad IPC, DSN/SES (legacy)
/app         CLI, job scheduler, portfolio
/bindings    pybind11
/bench       harness, fixture sets, KiCad-DRC judge
```

- **Language and build:** C++20 (C++23 where available), CMake, Ninja, `-Wall -Wextra -Werror`, sanitizers
  (ASan, UBSan, TSan) in CI, clang-tidy and clang-format.
- **Dependencies:** Clipper2, Boost.Geometry (`rtree`), oneTBB (or a small pool), fmt and spdlog,
  GoogleTest or Catch2, pybind11, and optionally CDT. All permissive licences; the project's own licence
  is a separate choice.
- **Memory:** arena allocation per transaction and stage, so rollback frees in O(1). Objects in
  struct-of-arrays layout for the R-tree and tile builds.

---

## 9. Evaluation

- **Fixture sets:**
  - PCBWorld D3-A (99 boards) and D3-B (10), from the PCBench corpus;
  - the ASP-DAC 2021 bm1–bm11 boards;
  - the PCBench gap set used for Freerouting;
  - synthetic stress boards (BGA escape, dense buses).
- **Metrics per board:** clean pass (EDA-tool DRC), completion, added DRC errors by type, vias, length,
  wall time and work units.
- **Gate for every change:** the same jar or binary flag-off vs flag-on in the same run; no board worse;
  zero new DRC errors; clean pass up or equal; time within budget.
- **Reference baselines:** Freerouting (current fork master), KiCad PNS via PCBWorld's harness, and the
  numbers reported by FanoutNet and PCBWorld.

---

## 10. Roadmap

| Phase | Deliverable | Exit gate |
|---|---|---|
| 1 | Kernel, model, rules compiler, KiCad I/O, DRC equal to `kicad-cli` on the unrouted fixtures | Same error list as KiCad on 109 D3 boards |
| 2 | Detailed router (gridless A*, multi-label) + transactions + cleanup, no global stage | Completion within 5% of Freerouting on D3, clean pass above it |
| 3 | Last gasp (strict, rip-up with negotiation, windows) | At least Freerouting's grid-fallback results on its 33 boards |
| 4 | Global NCR + corridors + negotiated repair | Better completion and time on bm1–bm11 |
| 5 | Escape planner for BGAs | bm boards with BGAs complete |
| 6 | Threading + determinism + portfolio | Same output for same seed at 1 and N threads; clean pass up at fixed time |
| 7 | KiCad IPC plugin, Python bindings, length and differential-pair rules | Plugin round-trip on PCBWorld boards |

---

## 11. Risks

| Risk | Mitigation |
|---|---|
| DRC equivalence with KiCad drifts across versions | Per-version rule tests against `kicad-cli` on a fixture set in CI |
| Gridless detailed search is slow on dense boards | Corridors bound the search; tile planes rebuilt incrementally; work budgets |
| Negotiation oscillates | History decay plus α scaling (BoxRouter), reroute caps, best-board keeping |
| Multi-threaded determinism costs speed | Batch barriers with deterministic merge; accept a modest loss |
| Escape planning does not fit irregular footprints | Fall back to negotiated-congestion escape, then to detailed routing |
| Scope creep into placement | Report placement limits only (analysis and cut diagnosis) |

---

## 12. References

See `robust_autorouting_research.md` for the full list with URLs and which sources were read in full.
The core ones:
- **Negotiated congestion and two-stage routing:** PathFinder (McMurchie & Ebeling, FPGA 1995); VPR/VTR
  source; AIR (Murray, Zhong & Betz, ASP-DAC 2020); FGR (Roy & Markov, TCAD 2008); BoxRouter 2.0 (Cho et
  al., ICCAD 2007); NTHU-Route 2.0; FastRoute and CUGR (code); TritonRoute-WXL (Kahng, Wang & Xu, TCAD
  2021); Dr. CU (ASP-DAC 2019).
- **PCB routing:** Contour (Dion & Monier, DEC WRL 95/3); Yu & Dai, cut-based routability (UCSC-CRL-97-07);
  Dayan & Dai, rubber-band layer assignment (UCSC-CRL-93-04); B-Escape (ISPD 2010); Yan & Wong escape flow
  (DAC 2009); Ozdal & Wong (TCAD 2006); Lin et al. (DAC 2021); Ma, Yan & Wong NCER (ISQED 2010).
- **Benchmarks and learned components:** FanoutNet (AAAI 2023); PCBWorld (arXiv 2607.05915); ASP-DAC 2021
  PCB benchmarks.
- **Implementations:** KiCad PNS router; gEDA toporouter; pcb-rnd route-rnd; Altium Situs documentation.
