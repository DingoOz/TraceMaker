# 12 — Decisions and risks

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Append new decisions at the end.

## Decisions

| # | Decision | Alternatives | Reason |
|---|---|---|---|
| D1 | Engine in C++20 + CUDA; viewer in TypeScript + WebGL2/WebGPU; harness and plugin in Python | Rust core (as KiCadRoutingTools); all-Python | CUDA ecosystem, existing C++ design, GCC 15 + nvcc available; Python is what `kicad-cli`/IPC tooling uses |
| D2 | Placement is in scope, with four explicit optimality levels | Report-only placement (clean-sheet doc) | User requirement; honest claims instead of "optimal" in name only |
| D3 | Analytic global placement, then SA/LNS, then CP-SAT windows | SA only (SA-PCB); GA only (Allegro X AI); RL (RL_PCB) | Analytic gives a provable relaxed optimum and a strong start; SA handles discrete PCB moves; CP-SAT gives exact local optima |
| D4 | Detailed routing first on a fine octilinear lattice with exact validation; gridless tile plane later | Gridless first (clean-sheet doc); Manhattan lattice (OrthoRoute) | Faster to make correct, GPU-friendly; Manhattan-only routing scores poorly on mixed boards |
| D5 | GPU accelerates fields, batches, broad-phases and placement — never the final legality decision | GPU end-to-end router | Quality and determinism; CPU reference for every kernel |
| D6 | Failure memory with four tiers (search, run, strategy, persistent) | History cost only | User requirement; each tier has published precedent |
| D7 | KiCad `.kicad_pcb` native I/O; DSN only for benchmarks/legacy | DSN/SES as main path | DSN loses rules (Freerouting evidence: 33/99 clean pass) |
| D8 | KiCad DRC (`kicad-cli`) is the benchmark judge | Own DRC only | Own DRC can be wrong in ways that hide errors |
| D9 | Integer fixed-point costs everywhere decisions are made | Floating point | Bit-identical CPU/GPU, deterministic atomics |
| D10 | Viewer in the browser, served by the engine; WebGL2 baseline, WebGPU when available | Native ImGui/Qt; WebGPU-only | Remote use from any machine, GPU rendering, no install; WebGPU is not yet default on Linux Firefox |
| D11 | Own CUDA kernels for placement and routing; no PyTorch/DREAMPlace runtime dependency | Embed DREAMPlace/Xplace | PyTorch wheels dropped Volta; CCCL 3/cuCollections partly untested on Pascal; BSD-3 code is ported, not linked |
| D12 | FLUTE (BSD-3, from OpenROAD `stt`) for Steiner trees | GeoSteiner | GeoSteiner is CC BY-NC (non-commercial) |
| D13 | Full placement aims for a 0.25 mm courtyard gap (board rule if present), falling back to KiCad's default of 0, then to keeping unplaceable parts at their input positions, then to the refine result | Always 0; always 0.25 mm | Most human boards are packed below 0.25 mm; the fallbacks keep full mode usable on dense boards |
| D14 | Global spreading uses SimPL rather than the electrostatic method | ePlace/DREAMPlace-style density | Simpler on CPU and adequate for PCB part counts (doc 04 §7) |
| D15 | Placement keeps a new placement only if it routes at least as well as the input under the same router budget (`--route-check`, `--mode auto`) | Trust HPWL | Lower HPWL from denser packing routed worse on PCBench (13 vs 15 fully routed boards) |
| D16 | Large boards (≥ 3M lattice points per layer) get two portfolio variants at double pitch | One pitch for all variants | Fine-pitch lattices are time-limited on large boards (P8000: 325 → 334 of 361 in 120 s) |
| D17 | Parallel tempering is *annealed* replica exchange: the ladder (refine: 16× → 1× the annealing temperature; full: √8× → 1/√8×) follows the annealing schedule, exchanges decided at a barrier from a dedicated stream | Fixed ladder T0 … 1e-4·T0 (textbook PT); independent runs only | The fixed ladder accepted ~3 % of exchanges and cost +5 % vs independent runs; the annealed ladder is −4 % (refine, 23 boards) at the same move budget and identical for any thread count (doc 04 §8) |
| D18 | Routability term is RUDY overflow + pin density on a ≤ 32×32 grid, integer nm, power nets excluded; the router loop shrinks bin capacity around failed connections | Electrostatic `R(x)` in global placement; airwire-crossing density only | Cheap to keep incremental and exact in the annealer (reference = from-scratch recompute); capacity shrinking is the PCB analogue of RePlAce cell inflation |
| D19 | Router-in-the-loop and ECO take the router as a callback (`RouteFn`) and accept a placement only if it leaves fewer connections unrouted (ties: shorter HPWL for seeds only) | Link tm::route into tm::place; accept on estimated overflow | Keeps tm::place independent of the router and testable with a synthetic router; the router's verdict, not an estimate, decides (rule 4) |
| D20 | Exact windows (doc 04 §3 F) by own branch and bound over candidate slots (own spot, other window parts' spots, HPWL-median spot; 4 rotations), not CP-SAT | OR-Tools CP-SAT | OR-Tools is not installed; B&B is exact over its candidate set, has a full-enumeration reference path, and is small enough to run inside annealing |
| D21 | `--route-check` restores a rejected placement by copying the input file | Re-saving the edited document | The editor edits the document in place, so re-saving wrote the *new* placement while reporting "kept input" (M7 bug, fixed 2026-10-03) |

## Risks

| Risk | Likelihood | Mitigation |
|---|---|---|
| `nvcc 12.4` does not accept GCC 15 as host compiler | **Confirmed** | Use `-ccbin g++-12` (installed; builds for `sm_60`/`sm_70`; ran on the P100). Stay on CUDA 12.x because CUDA 13 drops Pascal and Volta |
| GPUs are shared with other workloads (llama-server held almost all GPU memory on 2026-10-02) | **Confirmed** | Memory admission control, batch shrinking, CPU fallback with identical results (doc 07 §4) |
| DRC parity with KiCad drifts across KiCad versions | Medium | Pin a KiCad version per release; parity tests in CI |
| Lattice routing misses tight gaps | Medium | Pin-access points, half-pitch last-gasp windows, gridless cleanup, tile-plane arm |
| Placement quality judged subjectively by users (mechanical, thermal intent not in netlist) | High | `refine`/`eco` modes by default for placed boards; rule areas and locks honoured; full mode opt-in |
| Negotiation oscillates | Medium | History decay, α scaling, reroute caps, best-board keeping |
| Learned components overfit the benchmark | Medium | Held-out board sets; learned arms only via bandit, never mandatory |
| Viewer slows the engine | Low | Lock-free rings, coalescing, drop-frontier policy |
| Scope (placement + routing + GPU + viewer) is very large | High | Milestones each deliver a usable tool; GPU and learning are additive to working CPU paths |
