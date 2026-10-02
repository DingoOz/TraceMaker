# 11 — Implementation roadmap

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).
> Each milestone is sized for one to a few focused implementation sessions and ends with a **gate**: a
> test or benchmark that must pass before the next milestone starts. Visual feedback comes early (M3) so
> every later milestone can be watched while it is built.

| # | Milestone | Main deliverables | Gate |
|---|---|---|---|
| M0 | **Toolchain and skeleton** | CMake presets (`debug`, `release`, `asan`, `cpu-only`), CUDA build with a GCC host compiler that `nvcc 12.4` accepts, Catch2, clang-format/clang-tidy config (or GCC equivalents), `scripts/bootstrap.sh`, `kicad-cli` available (Docker image or native), fixture download script | `ctest` runs a CPU and a CUDA hello-kernel test on both GPUs; `kicad-cli version` works from the harness |
| M1 | **KiCad I/O and board model** | s-expression lexer/parser/writer; `.kicad_pcb`, `.kicad_pro`, `.kicad_dru`, `.kicad_sch` (netlist), `.kicad_mod`; board model with layers, stackup, footprints, pads, tracks, vias, zones, nets, net classes; DSN reader | Byte-identical round-trip of every fixture board (parse → write); netlist equals `kicad-cli sch export netlist` on all fixture projects |
| M2 | **Rules and DRC parity** | Rule compiler (net classes, board constraints, custom rules subset), spatial indices, own DRC with KiCad violation names | Same violation multiset as `kicad-cli pcb drc` on all unrouted and routed fixture boards (documented, tested exceptions only) |
| M3 | **Event bus, server, viewer v1** | Event types, ring buffers, WebSocket server, FlatBuffers schema, replay log; WebGL2 viewer (WebGPU backend optional): layers, pan/zoom, pads/tracks/vias/zones, ratsnest, dark theme | Viewer renders the largest fixture board at ≥ 60 fps; replay of a recorded log reproduces the final board |
| M4 | **First router (lattice A*)** | Rerun debug logging of searches (optional), transactions, scoring, union-find, bit-planes, A* + radix heap, insertion with exact validation, basic pull-tight; sequential routing in net order; `tracemaker route` CLI; bench harness v1 with KiCad-DRC judge and Freerouting runner | Routes the simple fixture set with 0 added KiCad DRC errors; harness produces the comparison table |
| M5 | **Failure memory T0–T2 + escalation R0–R3 + repair** | Failure records, Zobrist signatures, nogood store, history + targeted penalties, conflict graph, activity ordering, bandit; negotiated rip-up; repair windows; aggressor/victim repair | Completion ≥ Freerouting on the Freerouting test set; ablation shows each learning mechanism helps or is removed |
| M6 | **Global routing + GPU** | Port/adapt GAMER sweep and GGR pattern kernels from Xplace `gpugr` (BSD-3); tile graph, Steiner decomposition, CPU NCR, corridors; GPU pattern routing, GPU sweep maze routing, GPU cost-to-go fields, GPU DRC broad-phase | CPU and GPU results identical on every fixture; time-to-route reduced on dense boards with no quality loss |
| M7 | **Placement A–D** | Constraint extraction, decoupling detection, quadratic B2B (PCG), electrostatic global placement (CPU then GPU batched), rotation/side assignment, LP legalisation; placement report with optimality levels and lower bound | Placements are legal (zero courtyard overlaps, inside outline); HPWL ≤ SA-PCB baseline on the placement set |
| M8 | **Placement E–G + ECO** | GPU parallel-tempering SA, LNS, CP-SAT windows, routability loop; R4 exact window solve, R5 trial ordering, R6 cut proofs, ECO placement moves | Schematic-to-board set: clean pass ≥ 90%; `eco` mode closes boards that `none` mode cannot |
| M9 | **Escape planning** | Min-cost-flow escape, layer assignment, NC fallback, escape templates | BGA boards in the academic set complete |
| M10 | **Portfolio, determinism, knowledge base** | Successive-halving portfolio, deterministic sync points, persistent KB (T3), warm starts | Bit-identical output across thread counts and GPU on/off; clean pass up at fixed time budget |
| M11 | **KiCad integration** | IPC plugin (live board in pcbnew, one undoable commit), Python bindings, PCM package | Round-trip in KiCad on the demo boards |
| M12 | **Advanced rules and arms** | Differential pairs, length/skew tuning, blind/micro vias, gridless tile-plane arm, learned ordering/congestion models | Each feature gated by its own benchmark subset; no regressions |

## Working rules for implementation sessions

1. Read `PLAN.md`, the docs for the milestone, and `CLAUDE.md` first.
2. Every accelerated path (GPU, cache, index) lands **with** its reference path and an equivalence test.
3. Every change runs the quick benchmark subset; a regression on any board blocks the change.
4. Keep the viewer working: new stages emit events from day one.
5. Update the doc when the design changes, in the same change, and record the decision in
   [12-decisions.md](12-decisions.md).
