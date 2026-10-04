# TraceMaker — plan

> **Status:** design complete, implementation not started. **Date:** 2026-10-02.
> TraceMaker is a placement-aware, GPU-accelerated PCB autorouter for KiCad that learns from every failed
> attempt and shows its work live in a browser.

## Executive summary

1. **Input/output is native KiCad** (10.0.x files, `kicad-cli`, IPC plugin). No lossy DSN in the main path,
   because rules lost in translation are the main cause of DRC failures in existing autorouters.
2. **Placement is part of routing.** Analytic placement gives the exact optimum of the convex relaxation;
   legalisation is optimal for a fixed order; rotations, slot assignments and small windows are solved
   exactly (enumeration, Hungarian, CP-SAT); the whole board gets a certified lower bound and gap. During
   routing, the router may move unlocked parts, but only when it has *proved* a region cannot be routed.
3. **Routing is a staged pipeline** of published techniques: escape planning by min-cost flow, global
   negotiated-congestion routing (PathFinder/FGR/BoxRouter), optimal A* detailed routing in corridors,
   TritonRoute-style negotiated repair, a last-gasp ladder, cleanup, and sign-off with KiCad's DRC.
4. **Learning from failure has four tiers**: explain each failed search, update targeted history and
   conflict graphs, never repeat an identical failed attempt (Zobrist-keyed nogoods), choose strategies with
   bandits, and keep a persistent knowledge base across runs and boards.
5. **GPUs accelerate, never decide**: cost-to-go fields, sweep-based maze routing, placement density FFT,
   parallel-tempering annealing and DRC broad-phase run on the P100/V100, each with a bit-identical CPU
   reference. Integer costs make CPU and GPU agree exactly.
6. **The viewer** is a browser app (WebGL2, WebGPU where the browser has it) fed by a FlatBuffers WebSocket stream, with search frontiers,
   failure ghosts, heatmaps, footprint motion, a learning panel and a replay timeline.
7. **The benchmark** is Freerouting's own PCBench fixture set (~1,157 boards) judged by `kicad-cli pcb drc`.
   Freerouting v2.5.0-RC12 scores 74.6% clean pass there; TraceMaker 1.0 targets ≥ 85%.

## Reading order

| # | Document | Content |
|---|---|---|
| 1 | [docs/01-requirements.md](docs/01-requirements.md) | Requirements, scope, success criteria, glossary |
| 2 | [docs/02-architecture.md](docs/02-architecture.md) | System architecture, processes, pipeline, threading, determinism, repo layout |
| 3 | [docs/03-data-structures.md](docs/03-data-structures.md) | Geometry, spatial indices, hash tables, search structures, DRC, memory |
| 4 | [docs/04-placement.md](docs/04-placement.md) | Placement: optimality levels, analytic → legalise → anneal → exact, ECO moves |
| 5 | [docs/05-routing.md](docs/05-routing.md) | Routing stages, escalation ladder, cost model |
| 6 | [docs/06-failure-learning.md](docs/06-failure-learning.md) | Failure memory: records, nogoods, history, bandits, knowledge base |
| 7 | [docs/07-gpu.md](docs/07-gpu.md) | GPU workloads, kernels, determinism rules |
| 8 | [docs/08-kicad-integration.md](docs/08-kicad-integration.md) | File I/O, `kicad-cli`, IPC plugin, DRC judge |
| 9 | [docs/09-visualization.md](docs/09-visualization.md) | Live viewer, streaming, replay, visual design |
| 10 | [docs/10-benchmarking.md](docs/10-benchmarking.md) | Datasets, metrics, baselines, harness, gates |
| 11 | [docs/11-roadmap.md](docs/11-roadmap.md) | Milestones M0–M12 with gates |
| 12 | [docs/12-decisions.md](docs/12-decisions.md) | Decision log and risks |
| 14 | [docs/14-placement-test-plan.md](docs/14-placement-test-plan.md) | Placement test plan: levels, datasets, metrics, baselines, gates |
| 15 | [docs/15-component-rules.md](docs/15-component-rules.md) | **Partly built (M13 P0–P3):** component-aware layout rules (detect USB, Ethernet, crystals, regulators, RF, …; apply cited placement/routing rules); catalogue in [docs/component_rules.yaml](docs/component_rules.yaml); `tracemaker rules`, `--component-rules` (opt-in); status in §14 |

Background material:
- [cpp_autorouter_clean_sheet.md](cpp_autorouter_clean_sheet.md): the routing-stage design and the
  Freerouting evidence it rests on. Its routing stages are adopted; its "no placement" and "gridless first"
  choices are revised (decisions D2 and D4).
- [pcb-routing-optimization.md](pcb-routing-optimization.md): survey of placement/routing approaches and open
  source routers (SA-PCB, OrthoRoute, KiCadRoutingTools, tscircuit, PCBWorld, DreamerV3+FR).
- [research/](research/): verified facts on KiCad 10, its APIs, benchmark sets, and GPU techniques.
- `../pcbgolf/`: earlier Python placement experiments (constructive placer, crossing-minimising SA) and a
  2-layer C++ PathFinder grid router; useful as reference for KiCad coordinate conventions and move sets.

## Where to start implementing

Milestone **M0** in [docs/11-roadmap.md](docs/11-roadmap.md). Read [CLAUDE.md](CLAUDE.md) first.
