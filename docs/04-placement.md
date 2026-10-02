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
