# 04 — Component placement

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).

## 1. What "mathematically optimal" can honestly mean

Placement of rectangles to minimise wirelength without overlap is NP-hard (it contains bin packing and
the quadratic assignment problem), so no tool can promise a *global* optimum for a real board in useful
time. TraceMaker therefore makes four precise, checkable claims, and reports them per run:

| Level | Claim | How |
|---|---|---|
| L1 | **Global optimum of the convex relaxation.** The quadratic (B2B) wirelength problem without overlap constraints is strictly convex once one part is fixed, so its minimum is unique and found to tolerance. | Sparse SPD solve (PCG) |
| L2 | **Optimal for fixed topology.** Given the left/right and above/below order of parts, minimum-displacement legalisation and minimum-wirelength compaction are linear programs and are solved to optimality. | LP whose dual is min-cost flow |
| L3 | **Exact optimum of discrete sub-problems**: rotation of each part given its neighbours (enumerate 4 or 8 states), assignment of interchangeable parts to slots (Hungarian algorithm), side assignment for small clusters, and **whole windows of up to ~15–25 parts** with rotations (CP-SAT, proved optimal or gap reported). | Enumeration, Hungarian, CP-SAT |
| L4 | **Certified gap for the whole board.** The L1 linear-wirelength lower bound (an LP with no overlap constraints) bounds every legal placement from below; the report shows `HPWL_final / LB`. | LP / min-cost flow |

The report states, per run, which level each part of the placement reached, so "optimal" is never claimed
beyond what was proved. (The L4 bound is weak on dense boards; it is still a true bound.)

## 2. Inputs and constraints

Extracted from the KiCad project (doc 08):
- **Footprint geometry**: courtyard polygons (`F.CrtYd`/`B.CrtYd`; fallback: pad + fab bounding box with
  margin), pads per layer, through-hole flag (blocks both sides), 3D height if present (for keepouts).
- **Fixed objects**: locked footprints, user-fixed groups, mounting holes, edge connectors (default),
  board outline, cutouts, keepout rule areas with `footprints` disallowed.
- **Allowed states** per footprint: sides (SMD both, THT top only by default), rotations (0/90/180/270
  default; 45° steps opt-in; free angle opt-in), placement region (rule-area membership).
- **Courtyard clearance** from the board's `courtyard_clearance` rule and custom rules.
- **Netlist weights**: power/ground nets get low weight (they go to planes or pours); high-speed/diff-pair
  nets get high weight; nets with > 64 pins use a star/B2B model, never cliques.
- **Derived proximity groups** (constraints the placer infers, user can override):
  - **decoupling capacitors**: a two-terminal part whose nets are {power net, ground net} and which
    shares that power net with an IC power pin → soft constraint "within *d* of that pin", with the
    capacitor-to-pin assignment solved by the Hungarian algorithm (L3);
  - **crystal/oscillator** near its MCU pins; **termination resistors** near the driver or receiver
    (by net topology); **ESD/TVS** near the connector pin.
  - schematic **hierarchical sheets** and KiCad **groups/rooms** as cluster hints.

## 3. Pipeline

```
 constraints ─▶ (A) quadratic init ─▶ (B) electrostatic global ─▶ (C) side + rotation assignment
            ─▶ (D) legalisation (LP) ─▶ (E) detailed: SA / LNS (GPU, parallel tempering)
            ─▶ (F) exact windows (CP-SAT) ─▶ (G) routability loop with the router ─▶ ECO during routing
```

### (A) Quadratic initial placement — L1

- Bound-to-bound (B2B) net model (Spindler, Kraftwerk2): for each net, connect each pin to the net's
  extreme pins with weights `2 / ((p−1)·|x_i − x_j|)`; this reproduces HPWL exactly at the linearisation
  point.
- Solve `L_x x = b_x` and `L_y y = b_y` (Laplacian with fixed pins on the right-hand side) with
  Jacobi-preconditioned conjugate gradient (Eigen); re-linearise 5–10 times.
- Pins are offsets from part centres; the rotation is fixed at its current or default value here.
- Output is the unique global minimum of the relaxed problem: a good, deterministic starting point.

### (B) Nonlinear global placement — electrostatic (ePlace / RePlAce / DREAMPlace)

Objective: `min_x  W_WA(x; γ) + λ · D(x) + μ · R(x) + ν · P(x)`

- `W_WA`: weighted-average wirelength (smooth HPWL approximation with smoothing γ, annealed).
- `D`: electrostatic density penalty. Parts are positive charges; the potential comes from solving
  Poisson's equation on a bin grid with a DCT/DST (cuFFT on GPU, FFTW-style on CPU fallback).
- `R`: routability term: RUDY demand over the copper supply per bin (supply = layers × tracks per bin
  minus pad blockage), so congested bins push parts apart (cell inflation, as in RePlAce).
- `P`: proximity springs for the derived groups (decoupling, crystals) and the user's soft constraints.
- Nesterov's method with Lipschitz step estimation `‖v_k − v_{k−1}‖ / ‖∇f_k − ∇f_{k−1}‖` plus backtracking,
  and the diagonal preconditioner `(pin count + λ·area)` (ePlace); `λ` grows until the density overflow is
  below 10%. WA wirelength per net and axis:
  `WA_x = Σ x_i e^{x_i/γ} / Σ e^{x_i/γ} − Σ x_i e^{−x_i/γ} / Σ e^{−x_i/γ}`, γ annealed with overflow.
- **Net separation** (NS-Place): a margin term that pushes airwires of different nets apart where they would
  cross, which on PCBs reduces vias.
- Mixed-size by design: large ICs and connectors are just large charges.
- **Batched multi-start on GPU**: the problem is small for a PCB (tens to a few thousand parts), so a single
  run underuses a GPU. Run B = 32–256 placements at once with different seeds, net-weight perturbations and
  rotation guesses in one batched kernel set (one bin grid per start). Keep the best K by a fast
  estimate (HPWL + RUDY overflow + crossings) for the next stages.

### (C) Side and rotation assignment — L3 per part

- **Rotation**: coordinate descent. For each movable part, with all others fixed, enumerate all allowed
  rotations and keep the one minimising the local cost (pin-to-target HPWL + airline crossings). Each
  step is exactly optimal for that part; iterate to a fixed point (monotone, so it terminates).
- **Side** (two-sided boards): minimum-cut style partition with area capacity per side: start with
  FM (Fiduccia–Mattheyses) on the hypergraph where cutting a net costs a via estimate; exact by CP-SAT
  when the movable part count is ≤ ~40. THT parts are pinned to the top.

### (D) Legalisation — L2

1. Choose a **relative order** for every overlapping or near pair from the global result (horizontal if
   the x-overlap is smaller than the y-overlap, else vertical).
2. Build horizontal and vertical **constraint graphs**: `x_j − x_i ≥ (w_i + w_j)/2 + clearance`.
3. Solve `min Σ |x_i − x_i*|` (L1 displacement) subject to the graphs: an LP whose dual is a min-cost
   flow → exactly optimal for that order.
4. If the board outline makes it infeasible, relax the order for the parts in the infeasible cycle
   (reported by the flow solver) and repeat.
5. Non-rectangular courtyards and outlines: exact polygon check (Clipper2 Minkowski) after the LP;
   residual overlaps are fixed by a Tetris/Abacus pass with polygon tests.

### (E) Detailed placement — simulated annealing and large-neighbourhood search

SA is the proven workhorse for PCB placement (TimberWolf, OpenROAD SA-PCB; see
[`../pcb-routing-optimization.md`](../pcb-routing-optimization.md)). TraceMaker runs it *after* the analytic
stages, so it refines rather than searches blindly.

- **Moves**: shift (Gaussian, radius shrinks with temperature), swap two same-footprint parts, rotate,
  flip side, move a whole cluster, "pull toward the median of the connected pins" (greedy, exact for HPWL
  of one part in 1-D).
- **Cost** (incremental, O(degree) per move): weighted HPWL + α·airline crossings per layer group +
  β·RUDY overflow + γ·proximity-group violations + ∞ for overlap (moves are legal by construction:
  checked in a per-side bit-packed occupancy grid at 50 µm, then exactly by polygon).
- **Airline crossings** matter more on PCBs than on chips: every crossing on a 2-layer board costs a via
  or a detour. Crossings of MST airwires are counted with a uniform-grid segment hash (doc 03), updated
  incrementally per move.
- **Parallel tempering on GPU**: R replicas at geometric temperatures per surviving start; replicas
  exchange states (Metropolis criterion on the temperature swap). Each replica evaluates many
  independent candidate moves in parallel and applies a non-conflicting subset (moves whose parts and
  nets do not overlap) — deterministic because the subset is chosen by a fixed priority order.
- **LNS**: periodically destroy a region (k parts in a window or along a congested cut) and repair it by
  (F), keeping it if better.
- Alternative strategy: **NSGA-II** multi-objective GA (wirelength vs routability) as in the Cadence
  Allegro X AI work; included as a portfolio arm, not the default.

### (F) Exact windows — CP-SAT — L3

- Window of n ≤ ~20 parts (a decoupling cluster around an IC, a connector's support parts).
- Variables: integer position on a 50 µm (configurable) lattice, rotation literal, side literal;
  `NoOverlap2D` with optional intervals per rotation; HPWL linearised with min/max auxiliaries; boundary
  pins from the parts outside the window are fixed.
- Time-limited; the result is **proved optimal** or returned with its gap. Accepted only if the board
  score improves (transaction).

### (G) Routability loop

In `full` mode, after placement:
1. Run escape + global routing (fast, GPU) on the placement.
2. Inflate parts in overflowed tiles by `(overflow / capacity)^k` (RePlAce) and add cut-capacity springs
   for cuts proved over-full.
3. Re-run (B)…(E) incrementally from the current placement with anchor springs (to keep the result
   stable), at most 3–5 rounds or until overflow stops falling.
4. Continue into detailed routing.

## 4. ECO placement during routing (`eco` mode and all later routing)

When repair/last gasp proves a connection blocked (doc 05 §6 and doc 06 §3), it sends a
`PlacementRequest { cut geometry, deficit (tracks), connections affected, parts adjacent to the cut }`.
The ECO placer:
1. generates candidate moves for unlocked parts adjacent to the cut: shift along the cut normal by
   1…k track pitches, rotate 90/180°, swap with an equivalent part, move a passive to the other side,
   change the decoupling-cap slot;
2. ranks them by predicted capacity gain ÷ displacement (cheap geometric estimate), skipping moves recorded
   as failed for this cut signature (failure memory);
3. for the top N: opens a transaction, applies the move, legalises locally (D on the window), rips up the
   nets touching the moved parts, reroutes them plus the blocked connection, scores;
4. commits the best improving transaction, records the others as failures with their cause.

The bandit in doc 06 learns which move types pay off on this board.

## 5. Output

- New positions, rotations and sides written to the `.kicad_pcb` (only `at`, `layer` and flipped pad
  layers change; everything else is preserved byte-for-byte where possible).
- Placement report: per-level optimality claims, HPWL, lower bound and gap, crossings, overflow, moved
  parts, constraint violations.

## 6. Prior art used

| Source | Used for |
|---|---|
| GORDIAN, Kraftwerk2 (B2B), SimPL | Quadratic placement (A) |
| ePlace, RePlAce, DREAMPlace (BSD-3) | Electrostatic density, Nesterov, GPU FFT (B), cell inflation (G) |
| Abacus, Tetris; constraint-graph compaction | Legalisation (D) |
| TimberWolf; OpenROAD SA-PCB (BSD-3, archived 2026) | Annealing moves and schedule (E) |
| Allegro X AI GA; Ngô 2024 MIT thesis | NSGA-II portfolio arm (E) |
| **NS-Place** (ASP-DAC 2022): max-margin net separation, MILP legalisation; up to −50% vias, −79% DRVs | Net-separation term in the detailed cost (E) and as a cross-check for the LP legaliser (D) |
| ISPCBPlace (TCAD 2026): gradient placement on irregular boards with surface-layer routability | Outline-aware density and surface-layer routability term (B) |
| Xplace (BSD-3, ~3× per iteration vs DREAMPlace) | Kernel design reference for (B) |
| RL_PCB (Vassallo et al., DATE 2024) | Benchmark boards and a learned-placement baseline |
| Google OR-Tools CP-SAT (Apache-2.0) | Exact windows (F), side assignment (C) |
| Hungarian algorithm (Kuhn–Munkres) | Slot assignment (C, decoupling caps) |
| KiCadRoutingTools "placement quench / route-in-the-loop" | Prior art for the routability loop (G) |

## 7. Implementation status (M7, first version — 2026-10-02)

Code: `src/place/` (library `tm_place`, executable `tracemaker-place`, tests `tm_place_tests`, evaluation script
`src/place/eval_place.py`). No GPU code yet; everything below runs on the CPU and is deterministic for a given
seed, independent of the thread count (budgets are counted in moves; `--time` is only a safety stop).

```
tracemaker-place in.kicad_pcb -o out.kicad_pcb [--mode full|refine] [--seed N] [--threads N] [--runs N]
                 [--effort E] [--time S] [--alpha-cross-mm A] [--courtyard-clearance-mm C] [--move-connectors]
                 [--json report.json] [-v]
src/place/eval_place.py --route --truth --jobs 12 <PCBench board names>   # the table below
```

### 7.1 What is built

| Stage | File | As built |
|---|---|---|
| Problem | `problem.cpp` | Parts with courtyards per side (convex hull of the `F/B.CrtYd` graphics; no courtyard → pad box + 0.25 mm, or one box per pad for sparse footprints), through obstacles (PTH pad copper and holes, both sides), copper shapes with layers, net and required clearance (net class, local overrides, board minimum, 2 × solder-mask expansion so masks cannot bridge; with custom clearance rules their largest minimum, conservatively), pins per rotation, net weights (power-like names or > 30 pins: 0.1, else 1), outline = largest Edge.Cuts loop containing ≥ 80 % of the pads (gap tolerance up to 0.5 mm, else the Edge.Cuts bounding box), cut-outs, footprint keepouts, fixed board copper (tracks, vias, copper graphics and text). |
| Fixed parts | `problem.cpp` | locked, `board_only`, no pads, `REF**`, single-pad footprints (vias, test points, fiducials), mounting holes (`H*`/`MH*` without nets, `MountingHole` libs), footprints owning Edge.Cuts or keepouts, footprints touching routed copper, connectors (`J*`, `P<n>`, `CN*`, `USB*`) within 2 mm of the edge (`--move-connectors` frees them), and parts that already overhang the outline in the input. |
| (A) | `global.cpp` | B2B quadratic placement, Eigen CG with Jacobi preconditioner, 10 re-linearisations from the board centre, weak (1e-4) pull to the centre for strict convexity. |
| (B) | `global.cpp` | SimPL: lower-bound solve ↔ rough legalisation, anchor pseudo-nets of weight 0.1·k × the part's own net weight, until the bin overflow of the lower-bound placement is ≤ 10 % (≤ 40 iterations). Rough legalisation is recursive bisection over a capacity grid (free area per bin after the outline, keepouts and fixed parts), cutting parts at their area median and the region where its capacity is in the same proportion. **Why SimPL, not electrostatics:** boards have tens to a few hundred parts, so the per-iteration cost is negligible either way; SimPL needs no step-size control or γ annealing, reuses the B2B solver of (A), is deterministic, and its rough legaliser handles irregular outlines and fixed blockages through the capacity grid. The electrostatic method remains the plan for the GPU multi-start version. |
| (C) | `global.cpp` | Rotation coordinate descent (4 states per part, about the body centre, strict improvement only, so it terminates). |
| (D) | `legalize.cpp` | Largest-extent-first greedy: each part to the nearest legal lattice point (rings: 50–100 µm steps within 3 mm, 0.25 mm within 15 mm, 1 mm beyond), own rotation first. Fast path: conservative per-side occupancy raster (0.05 mm, 0.1 mm on large boards) with summed-area tables; every candidate is then confirmed by the exact test. Rip-up-and-re-place when a part finds no spot (evicts the least total area of movable parts), look-ahead for big parts (≥ 2 % of the board: refuse spots that leave another waiting big part no free spot), and retries with earlier failures first. In refine mode only parts that are illegal in the input move, at most 5 mm, and fall back to their (reserved) input spot. |
| (E) | `anneal.cpp` | Simulated annealing, every move checked exactly before it is applied (state always legal). Moves: shift (radius adapted to 44 % acceptance), weighted-median pull, rotate, swap same footprint, swap nearby parts. Integer cost = Σ w·HPWL + α·(crossings of MST airwires of different signal nets), α = 2 mm; crossings counted incrementally against a segment grid. `runs` independent runs (default one per thread, ≤ 16) with different Philox streams; best kept, ties to the lowest run. Budget: `effort × 4000 × movable parts` moves per run (effort 4 by default). |
| Bound | `lower_bound.cpp` | Exact LP lower bound of weighted HPWL (no overlap/outline constraints) via its min-cost-flow dual, integer costs, successive shortest paths. Two variants: input rotations, and "any rotation" (each pin takes its most favourable offset over the four rotations, per axis), which bounds every placement `tracemaker-place` can output. |
| Legality | `legality.cpp` | Exact integer tests (geom::Shape): courtyard vs courtyard with the courtyard clearance; through obstacles vs courtyards on both sides (0.1 mm) and each other (0.25 mm); copper vs copper of other parts and fixed copper (shared layer, different nets, max of both clearances); courtyards shrunk by 0.25 mm inside the outline and out of cut-outs; pads inside with the copper-to-edge clearance; courtyards out of footprint keepouts. |

**Courtyard clearance.** `full` uses the board's `courtyard_clearance` rule if present (custom rule or
`.kicad_pro`), else 0.25 mm; if some parts still cannot be placed it retries with KiCad's default (0), then keeps the
parts that found no position at their input positions (as fixed) and places the rest around them, and as a last
resort writes the `refine` result (all reported in `notes`). `refine` uses the rule or KiCad's default 0, so the
spacing a designer chose is never declared illegal. *This deviates from the single 0.25 mm default in the M7 brief;
it needs a row in doc 12.*

**Not implemented:** side flipping (parts keep their side; `BoardEditor` has no flip yet), the LP legaliser (L2),
CP-SAT windows and Hungarian slot assignment (L3 beyond single-part rotation), decoupling/crystal proximity
groups, RUDY/routability terms, the routability loop (G), GPU kernels and parallel tempering, ECO mode.

### 7.2 Optimality levels actually reached

| Level | Status |
|---|---|
| L1 | Computed but **not a bound**: the reported "L1 quadratic" value is the HPWL of the B2B optimum, i.e. the exact (CG-tolerance) minimum of a quadratic model linearised at the previous iterate. It approximates the relaxed HPWL optimum from above. The exact relaxed optimum is the L4 number with input rotations. |
| L2 | **Not reached.** Legalisation is greedy minimum-displacement per part, not the LP over a fixed order. |
| L3 | Only single-part rotation: after (C) each part's rotation is the exact HPWL optimum given its neighbours (full mode). The annealer moves on from that state, so the final placement carries no L3 claim. |
| L4 | **Reached.** `lb_any_rotation` is an exact LP optimum (integer min-cost flow) and a true lower bound on the weighted HPWL of every placement with the same fixed parts and sides. The report gives `HPWL_final / LB`. The bound is weak (median `HPWL_final / LB` 4.2 full, 4.8 refine on the boards below; ~0 on boards with few fixed parts, where everything may collapse to a point); it is still a true bound. |

### 7.3 Results on 23 PCBench boards (human placement = `unrouted.kicad_pcb`)

Seed 1, defaults (effort 4, 8 threads per board), machine shared with other jobs. HPWL is unweighted Σ HPWL in mm
over all nets with pins on ≥ 2 parts. "New DRC errors" = KiCad 10 `kicad-cli pcb drc` errors of type
courtyards_overlap, pth/npth_inside_courtyard, copper_edge_clearance (count increase) plus any other
inter-footprint error (clearance, shorting_items, solder_mask_bridge, …) whose item pair is not in the human
board's report. Routing: `tracemaker route --time 60 --threads 4` (engine build of 2026-10-02 13:22), routed /
connections. Every output re-reads with `tracemaker inspect`, and the KiCad round trip (`kicad_truth.py` vs
`inspect --json`) matches on 17 boards; the other 6 show exactly the same differences as the unmodified human
board (pre-existing reader issues: unnamed NPTH pad numbers, `~{…}` escaped references), so nothing is
introduced by placement.

¹ clearance fell back to 0 · ² some parts kept at their input positions · ³ refine result written

| Board | movable (full) | HPWL human | HPWL full | HPWL refine | LB (any rot.) | crossings h / f / r | new courtyard/edge/copper DRC errors f / r | routed human | routed full | routed refine |
|---|---:|---:|---:|---:|---:|---|---|---|---|---|
| 1Bitsy_1bitsy | 13 | 813 | 852 ² | 801 | 673 | 130 / 127 / 113 | 0 / 0 | 149/160 | 149/160 | 148/160 |
| AzizLight_AzizLight | 53 | 463 | 427 | 341 | 5 | 8 / 0 / 2 | 0 / 0 | 102/102 | 102/102 | 102/102 |
| ChirpHardware_chirp | 37 | 629 | 370 | 395 | 68 | 32 / 7 / 3 | 0 / 0 | 96/97 | 97/97 | 92/97 |
| ESP_nRF_Relay_Relay_WiFi_nRF24 | 12 | 642 | 299 | 297 | 68 | 18 / 4 / 5 | 0 / 0 | 36/36 | 34/36 | 34/36 |
| Hardware_Playground_Touch_Switch_2ch_PCB | 42 | 568 | 523 | 487 | 261 | 29 / 18 / 13 | 0 / 0 | 107/116 | 101/116 | 98/116 |
| IGN01A_IGN01A | 24 | 533 | 206 | 220 | 0 | 5 / 0 / 0 | 0 / 0 | 67/67 | 67/67 | 67/67 |
| LadybugLiteBlue_HW_LadybugBlueLite | 54 | 663 | 548 | 468 | 28 | 9 / 3 / 1 | 0 / 0 | 140/140 | 140/140 | 140/140 |
| Microdox-PCB_Microdox | 31 | 2788 | 1353 | 1839 | 75 | 87 / 26 / 36 | 0 / 0 | 223/223 | 223/223 | 223/223 |
| PiPlay_SDHat | 18 | 444 | 530 | 365 | 36 | 20 / 13 / 7 | 0 / 0 | 57/57 | 57/57 | 57/57 |
| RX5808_rx5808_4button | 51 | 774 | 640 | 542 | 65 | 65 / 15 / 24 | 0 / 0 | 126/126 | 125/125 | 123/123 |
| Solare-BQ24210_Solare-BQ24210 | 11 | 88 | 78 | 75 | 40 | 0 / 0 / 0 | 0 / 0 | 28/28 | 28/28 | 28/28 |
| a123-battery-integration_BCM | 62 | 1194 | 839 | 811 | 114 | 29 / 11 / 11 | 0 / 0 | 159/162 | 157/162 | 155/162 |
| beast-phat_beast-phat | 17 | 260 | 195 | 201 | 54 | 0 / 0 / 0 | 0 / 0 | 42/42 | 42/42 | 42/42 |
| bullion_bullion | 19 | 421 | 291 | 266 | 25 | 6 / 2 / 6 | 0 / 0 | 23/46 | 24/46 | 24/46 |
| domotics_out-board | 17 | 658 | 378 | 428 | 12 | 6 / 0 / 0 | 0 / 0 | 42/50 | 44/50 | 41/50 |
| esp32stack_esp32stack | 24 | 1040 | 792 | 780 | 462 | 24 / 5 / 4 | 0 / 0 | 92/95 | 94/95 | 94/95 |
| jadonk_PocketBone | 51 | 1229 | 1129 ² | 1059 | 326 | 124 / 48 / 43 | 0 / 0 | 202/202 | 202/202 | 202/202 |
| kitspace_hbridge_driver | 27 | 455 | 449 | 388 | 79 | 3 / 0 / 0 | 0 / 0 | 61/61 | 61/61 | 61/61 |
| kitspace_training_board_v02 | 66 | 1170 | 737 | 713 | 21 | 56 / 5 / 11 | 0 / 0 | 129/129 | 129/129 | 129/129 |
| nanoTracer_nanoTracer | 45 | 1301 | 994 | 886 | 8 | 22 / 11 / 7 | 0 / 0 | 112/112 | 106/112 | 108/112 |
| phone_amp_phone_amp | 10 | 412 | 411 ³ | 384 | 288 | 34 / 30 / 21 | 4 / 1 | 73/88 | 71/88 | 73/88 |
| scimpy_volumebuffer | 29 | 590 | 565 | 553 | 46 | 33 / 11 / 13 | 0 / 0 | 67/67 | 67/67 | 67/67 |
| uC3Moy_uC3Moy | 10 | 329 | 329 ² | 302 | 103 | 2 / 0 / 0 | 0 / 0 | 41/41 | 38/41 | 41/41 |

Totals: HPWL human 17 464 mm, full 12 930 mm (median ratio 0.83), refine 12 603 mm (0.74); airwire crossings
fall on almost every board. **No new courtyard, edge, clearance, short or mask-bridge errors on any board**
(silkscreen warnings do increase: reference text is not moved or checked). Routability did **not** improve:
routed connections human 2174/2247 (15 boards complete), full 2158/2246 (13), refine 2149/2244 (13). Lower HPWL
from denser packing does not buy routability with this router; the routability terms and loop (B `R(x)`, G) are
needed before placement can be expected to help routing. Run time per board 1–30 s (placement only).

Earlier iterations, kept here because they shaped the design: courtyard-only checks introduced shorts, clearance
and mask-bridge errors (pads outside courtyards, 2.5 mm high-voltage net classes, copper text), which led to the
copper model; footprints without courtyards (shield headers, BGAs) and broken outlines (gaps up to 0.5 mm, a
mounting-hole circle as the only loop) led to the fallbacks above.

### 7.4 Limitations and next steps

- Routability: add RUDY/crossing density to (B) and the router-in-the-loop (G); compare with routed completion,
  not HPWL.
- Full mode from scratch is weaker than refine (lower HPWL than refine on 5 of 23 boards; below the human HPWL on 21 of 23, refine on 23 of 23); the annealer is far from
  converged at effort 4 (effort 16 lowers HPWL another 5–10 %). Parallel tempering / GPU multi-start (doc 07).
- Courtyards are convex hulls (conservative for L-shaped courtyards); no 45° or free rotations; no flipping.
- Custom clearance rules are applied as a global maximum, not per condition.
- Zone fills are ignored (they are refilled after placement); silkscreen is ignored.
- Proximity constraints (decoupling caps, crystals) are not modelled: power-only parts can drift away from
  their IC.
