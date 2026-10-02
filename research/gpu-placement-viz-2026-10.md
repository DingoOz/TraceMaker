# Research: GPU routing, placement, learning and visualisation (checked 2026-10-02)

Sources: OpenAlex abstracts, GitHub READMEs/licences/source, fetched PDFs, caniuse data (no web search). *unverified* marks unconfirmed items.

**Platform:** CUDA 13.x supports only sm_75+, so P100 (sm_60) and V100 (sm_70) stay on CUDA 12.x. CCCL 3.x builds with CTK 12.x but sm_60/70 are untested there; the CCCL 2.x bundled with CUDA 12.4 is safer. PyTorch 2.11 dropped Volta from cu12.8/12.9 wheels and PyPI moved to cu13 (Turing+); only cu12.6 wheels still cover Pascal/Volta.

## 1. GPU global / maze routing

| Work | Core idea | Result | Code / licence |
|---|---|---|---|
| GAMER, ICCAD 2021 | Parallel 3D maze routing at pathfinding level in CUGR | 16× on coarse maze stage, no quality loss | Xplace `cpp_to_py/gpugr`, BSD-3 |
| GAMER, TCAD 42(2) 2023 | Multi-source/multi-target shortest path as alternating horizontal/vertical **sweeps** via prefix sum/min; one sweep O(log₂ n) | Coarse maze 19.85×, fine maze 2.59×, 2.7× overall vs CUGR | same |
| GGR, ICCAD 2022 | Full GR on GPU: Z-shape pattern routing + GAMER maze | >13× vs multithreaded CUGR | In Xplace (`gpugr`), BSD-3; deterministic with `N=0`; LEF/DEF only |
| FastGR, DATE 2022 / TCAD 2023 | CPU-GPU task-graph scheduler + GPU pattern routing | 2.307× scheduler, 10.877× GPU pattern routing, 2.426× overall | no public repo (*unverified*) |
| CUGR 2.0, DAC 2023 | DAG-based pattern routing, CPU only | quality-oriented | cuhk-eda/cu-gr-2, CU-SD licence |
| InstantGR, ICCAD 2024 | Batching by fine-grained 3D overlap (not bbox R-tree); node-level parallelism over DAG depth | 2.01× faster, 2.1% better than ISPD'24 1st; node-level kernel 10.7× vs net-level | cuhk-eda/InstantGR, BSD-3; default sm_80 |
| HeLEM-GR, ICCAD 2024 | Linearised exponential multiplier for overflow + CPU/GPU kernels | 1.62–2.07× faster, 4.8–5.8% better vs ISPD'24 top-3 | no code (*unverified*) |
| Sweep-sharing maze routing, DAC 2025 / TCAD 2026 | Several nets share one sweep; edge-level RRR | "significantly faster" | *unverified* |

## 2. GPU detailed routing / DRC / A*

| Work | What | Result | Code |
|---|---|---|---|
| X-Check, ICCAD 2022 | GPU DRC, parallel sweepline | significant speedup | — |
| OpenDRC, DAC 2023 | Per-layer BVH, row partitioning, edge-based kernels | beats MT CPU and prior GPU | github.com/opendrc/opendrc, MIT |
| E2E-Check, ASP-DAC 2024 | End-to-end GPU DRC incl. mask booleans | beats MT DRC | — |
| PDRC, DAC 2024 | Package-level **non-Manhattan** DRC, hierarchical interval lists | 30–50× vs two CPU checkers | — |
| GTA, ICCAD 2025 | GPU track assignment with rule-conflict LUT | 20× vs TritonRoute-WXL | — |

No published full GPU detailed router was found. Limits: GPU memory, largest-net bottleneck, launch overhead, irregular rule costs. Priority-queue A* parallelises poorly; GPU maze routers use BFS/sweep/Bellman-Ford relaxation.

## 3. GPU placement

| Tool | Notes | Licence |
|---|---|---|
| DREAMPlace (v2.0 … v4.3.1) | Default cc 6.0, tested 6.0/7.0/7.5; needs PyTorch (cu12.6 wheels only for Pascal/Volta) | BSD-3 |
| Xplace 1.0/2.0/3.0 | ~3× per GP iteration vs DREAMPlace; includes GGR | BSD-3 |

ePlace/RePlAce maths: WA wirelength `WA_x = Σxᵢe^{xᵢ/γ}/Σe^{xᵢ/γ} − Σxᵢe^{−xᵢ/γ}/Σe^{−xᵢ/γ}`; density via Poisson ∇²ψ = −ρ solved with DCT/DST; objective W + λN with λ rising as overflow falls; Nesterov with Lipschitz step estimate + backtracking; diagonal preconditioner (pin count + λ·area).

PCB placement 2020–2026: **NS-Place** (ASP-DAC 2022: max-margin net separation, coordinate descent, MILP legalisation; up to −25% routed WL, −50% vias, −79% DRVs); **RL_PCB** (DATE 2024, TD3/SAC, −17/−21% post-route WL vs SA, MIT); PCBAgent (ASP-DAC 2025); **ISPCBPlace** (TCAD 2026, gradient placement on irregular boards with surface-layer routability); DRLPlace (ASP-DAC 2026); SA-PCB (BSD-3). No dedicated CP-SAT PCB placement paper found (*unverified*).

## 4. Libraries

| Library | Licence | Notes |
|---|---|---|
| cuCollections | Apache-2.0 | Volta+; **Pascal partial** (no blocking-algorithm structures) |
| CCCL (CUB/Thrust/libcu++) | Apache-2.0 | v3.4.3; sm_60/70 untested on 3.x → use bundled 2.x with CUDA 12.4 |
| OR-Tools CP-SAT | Apache-2.0 | v9.15 |
| Clipper2 | BSL-1.0 | v2.0.1 |
| Boost.Geometry | BSL-1.0 | |
| ankerl::unordered_dense | MIT | v5.2.0 |
| FLUTE (Flute3 in OpenROAD `src/stt`) | BSD-3 | any degree |
| GeoSteiner 5.3 | **CC BY-NC 4.0** | non-commercial; avoid |
| Freerouting | GPL-3.0 | v2.4.1 |

## 5. Learning from failure

- PathFinder: `c_n = (b_n + h_n)·p_n`, `p_n = 1 + pres_fac·overuse_n` (pres_fac grows), `h_n ← h_n + hist_fac·overuse_n`.
- **TritonRoute-WXL marker cost (OpenROAD `drt` source):** each DRC marker adds +10 to a saturating per-node byte counter (`markerCostPlanar`/`markerCostVia`); decays ×0.95 per worker pass (`workerMarkerDecay`); node with counter > 0 adds `ggMarkerCost_ × edgeLength`; defaults `MARKERCOST = 32`, `ROUTESHAPECOST = 8`; fixed 50+-step schedule of {window size 7/5, offset, iterations, DRC/shape cost, marker cost 0→1×→2×, fixed-shape cost, decay, RipUpMode ALL/DRC/NEARDRC}; extra variants at marker cost ×½ and ×2.
- RL net ordering for Freerouting confirmed: Liao, Pan, Chiang, *Expert Systems with Applications* 2026, doi 10.1016/j.eswa.2026.131424 ("DreamerV3+FR"): 96% completion, 21% less training time than DQN.
- CDCL/nogood learning in VLSI/PCB detailed routing: no paper found (closest: conflict-driven ASP for optical routing, 2015) → open design opportunity.

## 6. Visualisation (Oct 2026)

| Item | Status |
|---|---|
| WebGPU Chrome/Edge | Default since 113 on Windows/macOS/ChromeOS; **Linux from 144 depends on hardware/drivers** |
| WebGPU Firefox | Windows since 141; macOS 26 Apple Silicon since 145; **behind a flag on Linux through 160** |
| WebGPU Safari | 26.0 on macOS 26 / iOS 26 (partial) |
| deck.gl 9.4.0 | WebGPU for all layers but "experimental, not production"; MIT |
| PixiJS 8.22.0 | WebGPU renderer; auto-detect prefers WebGL; MIT |
| Rerun 0.38.1 | MIT/Apache-2.0; C++17 SDK; web viewer WebGPU with WebGL fallback; "expect breaking changes" |
| FlatBuffers 25.12.19 | Apache-2.0; C++ and TS codegen |

Recommendation: WebGL2 baseline, WebGPU optional; Rerun for quick debug views; custom WebSocket + FlatBuffers viewer for the product.

## References
Xplace https://github.com/cuhk-eda/Xplace · InstantGR https://github.com/cuhk-eda/InstantGR (doi 10.1145/3676536.3676787) · GAMER doi 10.1109/iccad51958.2021.9643563, 10.1109/tcad.2022.3184281 · GGR doi 10.1145/3508352.3549474 · FastGR https://yibolin.com/publications/papers/ROUTE_DATE2022_Liu.pdf · HeLEM-GR doi 10.1145/3676536.3676650 · GTA https://yibolin.com/publications/papers/ROUTE_ICCAD2025_Zhao.pdf · CUGR2 https://github.com/cuhk-eda/cu-gr-2 · sweep-sharing doi 10.1109/tcad.2026.3737066 · OpenDRC https://github.com/opendrc/opendrc · PDRC doi 10.1145/3649329.3657367 · DREAMPlace https://github.com/limbo018/DREAMPlace · ePlace doi 10.1145/2699873 · NS-Place doi 10.1109/asp-dac52403.2022.9712480 · RL_PCB https://github.com/LukeVassallo/RL_PCB · ISPCBPlace doi 10.1109/tcad.2026.3675948 · SA-PCB https://github.com/The-OpenROAD-Project-Attic/SA-PCB · DreamerV3+FR doi 10.1016/j.eswa.2026.131424 · TritonRoute-WXL doi 10.1109/tcad.2021.3079268, OpenROAD `src/drt` · PathFinder doi 10.1109/fpga.1995.242049 · cuCollections https://github.com/NVIDIA/cuCollections · CCCL https://github.com/NVIDIA/cccl · OR-Tools https://github.com/google/or-tools · GeoSteiner licence http://www.geosteiner.com/LICENSE · PyTorch 2.11 notes https://github.com/pytorch/pytorch/releases/tag/v2.11.0 · caniuse WebGPU https://caniuse.com/webgpu · deck.gl, PixiJS, Rerun, FlatBuffers GitHub repos.
