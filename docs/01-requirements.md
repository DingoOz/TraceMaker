# 01 — Requirements, scope and success criteria

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).

## 1. Product statement

TraceMaker is a **placement-aware PCB autorouter for the KiCad stack**. It reads a KiCad project
(schematic, board, project rules), places or improves the placement of components from the known
connectivity, routes every connection with published state-of-the-art algorithms, learns from every
failed attempt, and writes a board that passes KiCad's own DRC. A live, GPU-rendered viewer shows the
whole process as it happens.

## 2. Functional requirements

| ID | Requirement | Notes |
|---|---|---|
| F1 | Read a KiCad project: `.kicad_pro`, `.kicad_sch` (hierarchical), `.kicad_pcb` | KiCad 10.0.x (board format 20260206), 9.0 readable; native s-expression parsers; `kicad-cli` used for netlist cross-checks |
| F2 | Read every design rule KiCad applies: net classes, custom rules (`.kicad_dru`), stackup, board constraints, keepouts/rule areas, mask rules | "Never route on rules that could not be read" — warn and fall back to conservative values |
| F3 | Start from (a) a board with footprints already placed, (b) a board with footprints piled outside the outline (KiCad "Update PCB from schematic"), or (c) a schematic only plus footprint libraries | (c) builds footprints from library `.kicad_mod` files |
| F4 | **Place** movable components: position, rotation (90° steps by default, arbitrary optional), side (top/bottom where allowed) | Locked footprints, connectors marked fixed, mounting holes never move |
| F5 | Placement is **optimal from the known connections** in a precise, reported sense: exact optimum of each convex sub-problem, exact optimum of small sub-problems with a certificate, and a reported lower bound / gap for the whole | See [04-placement.md §1](04-placement.md) for the honest definition |
| F6 | **Move components during routing** (placement ECO) when the router proves a region cannot be routed | Bounded, transactional, never touches locked parts |
| F7 | Route all nets on 1–32 copper layers: tracks (octilinear first, arcs in cleanup), through/blind/buried/micro vias, plane connections, net-class widths and clearances, differential pairs and length rules (later phase) | |
| F8 | "Try optimal first, learn from failure": every connection is first attempted with an optimal search under the current cost model; every failure is recorded with its cause and used to change the next attempt | See [06-failure-learning.md](06-failure-learning.md) |
| F9 | Output: routed `.kicad_pcb` (placement + tracks + vias), optional Specctra SES, JSON report, event log for replay | Writes never modify unrelated file content |
| F10 | KiCad integration: CLI, KiCad IPC API plugin (live in pcbnew), Python bindings | |
| F11 | Real-time visual display of placement and routing, viewable locally or remotely in a browser | See [09-visualization.md](09-visualization.md) |
| F12 | Self-benchmarking against public board sets (Freerouting tests, KiCad demos, academic PCB benchmarks) with KiCad DRC as the judge, and comparison against Freerouting | See [10-benchmarking.md](10-benchmarking.md) |

## 3. Non-functional requirements

| ID | Requirement |
|---|---|
| N1 | **Correctness first**: the output must pass `kicad-cli pcb drc` with zero *added* errors (relative to the input board) |
| N2 | **Anytime**: a best-so-far board is always available and can be written at any moment |
| N3 | **Deterministic**: same input + settings + seed ⇒ bit-identical output, at any thread count, with or without GPU (GPU kernels use integer or order-independent reductions) |
| N4 | **Fast** on the target machine (56 cores, 150 GB RAM, Tesla P100 sm_60 + V100 sm_70, CUDA 12.4): seconds for small boards, minutes for dense 8–12 layer boards |
| N5 | Exploits memory and GPUs where it buys speed **without loss of quality**: every accelerated path has a CPU reference implementation and a test that both agree |
| N6 | Graceful without a GPU (CPU fallback) and without KiCad installed (own DRC; KiCad DRC only for sign-off) |
| N7 | Observable: every stage emits structured events (for the viewer, the replay log and the report) |
| N8 | Licences: core dependencies permissive (MIT/BSD/Boost/Apache-2.0/MPL-2.0). TraceMaker's own licence is the user's choice (GPL-3.0-or-later matches KiCad and Freerouting) |

## 4. Non-goals (version 1)

- Signal-integrity sign-off (impedance solving, crosstalk simulation). Rules from KiCad are honoured;
  physics is not simulated.
- Schematic editing. Pin/gate swapping that changes the schematic is a later, opt-in feature with
  back-annotation.
- Replacing KiCad's interactive router. TraceMaker is batch/ECO; users finish by hand in pcbnew.
- Topological (rubber-band) routing as the core engine. It may appear later as one portfolio strategy.

## 5. Success criteria (measured by the benchmark harness)

| Metric | Target for 1.0 |
|---|---|
| KiCad-DRC clean pass (100% routed, 0 added errors) on the routing benchmark sets with fixed placement | ≥ Freerouting on every set, strictly higher on the aggregate. Concrete bar: Freerouting v2.5.0-RC12 reaches **74.6%** clean on the ~1,157-board PCBench set (its own nightly report, 2026-09-30); TraceMaker 1.0 targets **≥ 85%** |
| Completion (connections routed) | ≥ Freerouting on ≥ 95% of boards; never worse by more than 1 connection |
| Wall time at equal or better quality | ≤ Freerouting on ≥ 80% of boards |
| Placement + routing from scratch (schematic-only inputs) | Clean pass on ≥ 90% of the "place-and-route" set within 10 min each |
| Determinism | Bit-identical output across 1/8/56 threads and GPU on/off for every benchmark board |

## 6. Glossary

| Term | Meaning |
|---|---|
| Connection | A two-pin sub-problem of a net (an edge of the net's Steiner or spanning tree) |
| Clean pass | Fully connected **and** zero added KiCad DRC errors |
| Corridor | The set of global-routing tiles (per layer) a connection is guided through |
| Transaction | A copy-on-write branch of the board model that is committed only if the score improves |
| Failure record | A structured description of why one attempt failed (cause, blockers, region signature, strategy) |
| Nogood | A recorded (region signature, strategy) pair known to fail, so it is not retried unchanged |
| ECO move | A small placement change requested by the router (shift, rotate, flip, swap) |
| Work unit | A deterministic unit of effort (search expansions, DRC checks) used for budgets instead of wall time |
