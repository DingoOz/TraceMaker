# 02 — System architecture

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).
> This document supersedes §3, §6 and §8 of [`../cpp_autorouter_clean_sheet.md`](../cpp_autorouter_clean_sheet.md)
> and keeps its routing stages (§5) as the routing core. The main changes: placement is in scope,
> failure learning is a first-class subsystem, GPUs are used, and there is a live viewer.

## 1. Big picture

```
 ┌──────────── KiCad project ─────────────┐                 ┌──────────── Viewer (browser) ─────────────┐
 │ .kicad_pro  .kicad_sch  .kicad_pcb     │                 │ WebGL2/WebGPU · timeline · inspectors    │
 │ .kicad_dru  footprint libs             │                 └─────────────▲─────────────────────────────┘
 └──────┬─────────────────────────────────┘                               │ WebSocket (FlatBuffers deltas)
        │ io::kicad (s-expr)  /  IPC API (live)                            │
 ┌──────▼──────────────────────────────────────────────────────────────────┴───────────────────────────┐
 │ tracemakerd  (C++20 + CUDA engine, one process)                                                     │
 │                                                                                                     │
 │  Ingest ─▶ Rules compile ─▶ Analysis ─▶ PLACE ─▶ Escape ─▶ Global NCR ─▶ Detailed ─▶ Repair ─▶ Last │
 │                                ▲          │                                     │           gasp    │
 │                                │          └────────── ECO placement ◀───────────┴── proofs of ──┘   │
 │                                │                       moves (transactional)       unroutability    │
 │                                └───────── Failure memory (in-attempt / in-run / persistent KB) ─────┤
 │                                                                                                     │
 │  Board model: geometry + spatial indices + transactions   │  Scheduler: budgets, portfolio, seeds   │
 │  GPU service: maze/BFS fields, density FFT, DRC broadphase │  Event bus ─▶ viewer, replay log, report│
 └─────────────────────────────────────────────────────────────────────────────────────────────────────┘
        │ io::kicad writer  /  IPC commit                       │ kicad-cli pcb drc (sign-off judge)
        ▼                                                       ▼
   routed .kicad_pcb                                       report.json + events.tmlog
```

## 2. Process model

| Process | Language | Role |
|---|---|---|
| `tracemakerd` | C++20 + CUDA | The engine. Runs one job or serves many over a local API. Embeds the HTTP/WebSocket server for the viewer. |
| `tracemaker` | C++ (thin) | CLI front end: `tracemaker route|place|bench|serve|replay`. Links the engine in-process. |
| Viewer | TypeScript + WebGL2/WebGPU | Static web app served by the engine. Works on the server's own display or remotely. |
| KiCad plugin | Python (`kicad-python`/IPC) | Launched from pcbnew; streams the board to the engine and applies results as one undoable commit. |
| Bench harness | Python 3 + `kicad-cli` | Drives fixture sets, calls KiCad DRC as the judge, runs Freerouting baselines, writes result tables. |

One engine process keeps the board model, GPU contexts and memory pools warm. The CLI is the same
library without the server.

## 3. The board model and transactions

All stages share one **board model** and change it only through **transactions** (copy-on-write
branches, commit only if the score improves, O(changes) rollback). This is unchanged from the clean-sheet
doc §4.4 and is the single most important structural decision.

Additions for placement:
- Footprints are first-class movable objects. A **footprint move** is a transaction operation that
  re-transforms the pads and graphics, invalidates the attached connections' routing, and marks the
  old and new footprint regions dirty in every spatial index.
- **Placement state** (position, rotation, side per footprint) is versioned with the board, so a
  placement ECO and the reroute it triggers commit or roll back together.

Score (lexicographic, smaller is better), computed incrementally over dirty regions:

```
score = ( added_drc_errors,           // hard: must be 0 to be a clean pass
          unrouted_connections,
          unrouted_nets,
          rule_soft_violations,       // length / skew targets
          via_count * w_via + wirelength_mm + w_bend * bends + w_place * placement_penalty )
```

## 4. Pipeline and control flow

The pipeline is a **state machine driven by the scheduler**, not a fixed sequence. Normal flow:

1. **Ingest** project → board model, netlist, rules (doc 08).
2. **Rules compile** → constant-time rule queries per (object class, object class, layer).
3. **Analysis** → routability report: RUDY demand, airline crossings, pin-escape capacity, dead pins.
4. **Placement** (if allowed) → global analytic, legalisation, discrete optimisation, exact windows (doc 04).
5. **Routing** → escape, global negotiated-congestion routing, detailed, repair, last gasp, cleanup (doc 05).
6. **Sign-off** → own DRC, then optionally `kicad-cli pcb drc` (doc 08).

Feedback edges (the reason it is a state machine):
- **Repair/last gasp → ECO placement**: when a connection is *proved* blocked by a cut whose capacity is
  below demand on every layer, the router asks the placer for moves that raise that cut's capacity.
- **Global routing → placement** (full-placement mode only): an overflow map inflates the footprints in
  hot regions and re-runs incremental analytic placement (routability-driven placement).
- **Every stage → failure memory → every later attempt** (doc 06).
- **Portfolio**: the scheduler runs several variants (seeds, schedules, orderings) in sibling branches and
  gives more budget to the leaders (successive halving).

## 5. Placement modes

| Mode | What may move | Typical use |
|---|---|---|
| `none` | Nothing. Pure router. | Benchmarking against Freerouting, user-placed boards |
| `eco` (default for placed boards) | Unlocked parts, by small moves only, only on proof of unroutability | Finishing a nearly routable board |
| `refine` | Unlocked parts, by local optimisation from the current placement | Improving a hand placement |
| `full` (default for unplaced boards) | All unlocked parts, global placement from scratch | Schematic-to-board |

Locked footprints, footprints with the `fixed` user attribute and parts in user-fixed groups never move in
any mode. Edge connectors and mounting holes are fixed by default (configurable).

## 6. Execution model

### 6.1 Threads

- **CPU**: one work-stealing pool (oneTBB). Stages submit region-partitioned work items; overlapping items
  never run together. A deterministic scheduler orders item starts and merges results in a fixed order.
- **GPU**: a `GpuService` owns one CUDA context per device, streams per stage, and pinned host buffers.
  Each GPU job is pure (inputs → outputs), so the CPU reference path can be swapped in and compared.
- Default device roles: **V100** for routing fields and DRC broad-phase, **P100** for placement (density
  FFT, batched multi-start). Both are used for the portfolio when the other role is idle.

### 6.2 Determinism

- One `uint64` job seed; a counter-based RNG (Philox4x32 or SplitMix64) keyed by (seed, stage, item id),
  identical on CPU and GPU.
- Stable tie-breaks by object id then coordinates. Never iterate hash containers in output-affecting order.
- GPU reductions on costs use integer fixed-point (costs are `int64` micro-units) so atomics are
  order-independent. Floating point is allowed only where the result is snapped before it affects a
  decision (placement positions are snapped to a 1 µm grid each iteration).
- Budgets are **work units**, not wall time, at decision points. Wall time only stops the job early.
- Portfolio variants are defined by (settings, variant index), never by the thread count; threads only schedule
  them, and the best variant is chosen by a total order ending in the variant index, so a work-budget run is
  bit-identical at any thread count (router: `--variants`, default all eight with `--work`; D47).

### 6.3 Budgets

The scheduler gives each stage a deadline plus a work quota; callees check the work quota in inner loops.
Default split of the remaining time: placement ≤ 30% (full mode), escape 5%, global 10%, detailed 25%,
repair 15%, last gasp ≤ min(120 s, 50% of remainder), cleanup ≥ 5% reserved.

## 7. Event bus

Every stage emits typed events (doc 09 lists them): `NetRouted`, `SearchFrontier`, `RipUp`, `FailureRecorded`,
`FootprintMoved`, `CongestionMap`, `DrcMarker`, `StageBegin/End`, `ScoreChanged`. The bus fans out to:
1. the **viewer stream** (coalesced to ≤ 60 Hz, level-of-detail filtered);
2. the **replay log** (append-only, zstd-compressed FlatBuffers, keyframe every N events);
3. the **report builder**.

Emitting must never block a stage: lock-free single-producer ring buffer per thread, drained by one
publisher thread; if the viewer falls behind, frontier events are dropped (state events never are).

## 8. Repository layout

```
TraceMaker/
  CMakeLists.txt  CMakePresets.json  cmake/
  src/
    core/        ids, arena allocators, RNG, fixed-point, budgets, logging, events
    geom/        int64-nm kernel, predicates, octilinear ops, Clipper2 wrappers
    model/       board, layers, stackup, footprints, nets, net classes, transactions, scoring
    rules/       rule compiler (net classes, .kicad_dru, board constraints), rule query object
    index/       R-trees, uniform spatial hash, bit-packed layer grids, tile planes, congestion maps
    io/kicad/    s-expression lexer/parser/writer; .kicad_pcb/.kicad_sch/.kicad_pro/.kicad_dru/.kicad_mod
    io/dsn/      Specctra DSN reader / SES writer (benchmarks and legacy)
    analysis/    RUDY, crossings, escape capacity, cut (Maley) analysis, routability report
    place/       analytic (quadratic + electrostatic), legaliser, discrete optimiser, exact (CP-SAT), ECO
    route/escape/ route/global/ route/detail/ route/repair/ route/cleanup/
    learn/       failure records, nogood store, history maps, activity, bandits, persistent KB
    drc/         rule checks equivalent to KiCad's
    gpu/         CUDA kernels + host wrappers; each with a CPU reference
    sched/       job scheduler, portfolio, stage state machine
    server/      HTTP + WebSocket server, FlatBuffers schema, replay log
    app/         CLI entry points
  bindings/python/   pybind11 module `tracemaker`
  viewer/            TypeScript app, WebGL2 + WebGPU backends (Vite)
  kicad_plugin/      IPC plugin (Python, PCM package)
  bench/             harness, fixture manifests, KiCad-DRC judge, Freerouting runner, report generator
  tests/             unit, property, golden-file, CPU-vs-GPU equivalence
  third_party/       pinned via CMake FetchContent / vcpkg manifest
  docs/  research/
```

## 9. Technology choices

| Concern | Choice | Why |
|---|---|---|
| Engine language | C++20 (GCC 15), CUDA 12.4 (`sm_60;sm_70`) | Performance, CUDA, matches the existing design doc |
| Build | CMake ≥ 3.28 + Ninja, presets `debug`, `release`, `asan`, `tsan`, `cpu-only` | |
| Dependencies | Clipper2, Boost.Geometry, oneTBB, Eigen, OR-Tools (CP-SAT), ankerl::unordered_dense, fmt, spdlog, FlatBuffers, zstd, uWebSockets (or Boost.Beast), SQLite, Catch2, pybind11, CUB/Thrust | All permissive licences |
| Viewer | TypeScript, WebGL2 baseline + WebGPU backend, Vite | Remote-capable, GPU-rendered, no install on the viewing machine |
| Harness | Python 3.12+ (uv-managed venv), pandas, matplotlib | Matches `kicad-cli` and Freerouting tooling |

Note: the machine has GCC 15 but no clang. CUDA 12.4's `nvcc` supports host GCC only up to 13, so the
build must use a GCC 13 host compiler for `.cu` files (`-ccbin`), or upgrade CUDA. Verify this first
(roadmap M0).
