# 07 — GPU acceleration

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).

## 1. Hardware facts that shape the design

| Device | Arch | Memory | Implications |
|---|---|---|---|
| Tesla P100 PCIe 12 GB | Pascal, `sm_60` | HBM2 ~550 GB/s | fp32/fp64 strong; no tensor cores; good for FFT and stencil sweeps |
| Tesla V100 PCIe 16 GB | Volta, `sm_70` | HBM2 ~900 GB/s | Independent thread scheduling; tensor cores (fp16) not needed here |

- Build for `sm_60;sm_70` with CUDA 12.x. **CUDA 13 removed offline compilation for Maxwell, Pascal
  and Volta**, so the project stays on CUDA 12.x while these GPUs are in use.
- No bf16, no `sm_80+` features (async copy, `cuda::pipeline` hardware path). Integer atomics and warp
  intrinsics (`__shfl_sync`, `__ballot_sync`) are the main tools.
- The GPU path must never be required for correctness (CPU fallback, `cpu-only` preset).
- **Verified on 2026-10-02:** `nvcc 12.4` rejects GCC 15 (`gcc versions later than 13 are not supported`)
  and builds for `sm_60` + `sm_70` with `-ccbin g++-12`; the test kernel ran correctly on the P100 (the V100 was full). The build uses
  `CMAKE_CUDA_HOST_COMPILER=g++-12`; host C++ code may still use GCC 15, so CUDA-facing headers must
  compile under both.
- **The GPUs are shared.** On 2026-10-02 a `llama-server` process held 11.4 of 12 GB on the P100 and 16.1 of
  16 GB on the V100, and a 128-byte `cudaMalloc` failed on the V100. TraceMaker must treat GPU memory as
  scarce and variable (see §4).
- CUDA's default device order is fastest-first (V100 = device 0) while `nvidia-smi` lists the P100 first.
  Select devices by name/UUID, never by index.

## 2. What runs on the GPU

| Workload | Kernel design | CPU reference | Expected gain |
|---|---|---|---|
| **Placement density** (doc 04 B) | Bin charge scatter (atomic int fixed-point), DCT/DST via cuFFT, field gather | FFTW-style CPU DCT (pocketfft) | Large for batched multi-start (B = 32–256 starts at once) |
| **Placement SA** (doc 04 E) | Parallel tempering: one block per replica; per-move incremental HPWL by warp over net pins; conflict-free move subsets | Same algorithm, sequential | Many replicas per second instead of one |
| **Global pattern routing** | One thread per (connection, pattern, layer pair): L/Z-shape costs from prefix sums of edge cost along rows/columns | CPU loop | Thousands of connections per launch |
| **Global maze routing** (GAMER-style) | Alternating horizontal/vertical sweeps with parallel prefix-min (scan) per row/column per layer, plus via relaxation between layers, iterated to convergence. Batches formed by **fine-grained 3-D overlap of routed tiles** rather than bounding boxes (InstantGR), so more connections share a launch | CPU A* | GAMER: 19.85× coarse / 2.59× fine maze vs CUGR, no quality loss |
| **Cost-to-go fields** (doc 05 §5.2) | Same sweep kernels on a detailed window, multi-layer, from the target | CPU Dijkstra | Turns A* into near-linear expansion along the optimal path |
| **Free-space BFS** (last gasp, strict mode) | Bit-parallel wavefront: 32/64 cells per thread word, dilate-and-mask per step | CPU bit-parallel BFS | Whole-board reachability in milliseconds |
| **Conservative rasterisation** of obstacles | One thread per primitive × covered tiles; atomic OR into bit-planes | CPU rasteriser | Board-scale planes rebuilt fast after placement moves |
| **DRC broad-phase** | LBVH (Karras 2012) built per stage; parallel traversal emits candidate pairs; exact narrow phase on CPU or GPU with the same integer predicates | Uniform spatial hash | Full-board sign-off in well under a second |
| **RUDY / pin-density / crossing maps** | Scatter kernels | CPU | Cheap; keeps placement loop interactive |

## 3. Correctness and determinism rules

1. Every kernel has a CPU reference with the same integer arithmetic; a test runs both on random and
   fixture inputs and requires identical output.
2. Costs and charges are `int64`/`int32` fixed-point, so atomic accumulation is order-independent.
3. Batch selection (which connections/moves run together) is computed on the CPU in a deterministic order.
4. Floating-point stages (placement Nesterov steps) snap positions to a 1 µm grid each iteration and use
   deterministic reductions (fixed-order tree reductions, no float atomics).

## 4. Service design

- `GpuService` owns per-device context, streams, `cudaMallocAsync` memory pools, and pinned staging buffers.
- Jobs are pure functions on device buffers: `FieldJob`, `SweepRouteJob`, `DensityJob`, `AnnealJob`,
  `BroadphaseJob`. The scheduler queues them; results return through futures.
- Device roles default to V100 = routing/DRC, P100 = placement; a job may run on either.
- Graceful degradation: if a device is busy or absent, the CPU reference runs.
- **Memory admission control**: at start and before each large job, query `cudaMemGetInfo`; size batches to
  the free memory minus a safety margin; on `cudaErrorMemoryAllocation` shrink the batch and retry, then fall
  back to CPU for that job. Results are identical either way (rule §3.1), so this never changes output.
- Settings: `--gpu auto|off|<uuid,...>` and `--gpu-mem-limit` per device; the report records which jobs ran
  where.

## 5. Libraries

- **CUB/Thrust from the CCCL 2.x bundled with CUDA 12.4.** CCCL 3.x builds with CTK 12 but is untested below
  `sm_75`.
- **cuFFT** for placement density.
- **In-house linear-probing device hash table.** cuCollections supports Pascal only partially (no blocking
  structures).
- **No PyTorch or DREAMPlace runtime dependency.** Current PyTorch wheels dropped Volta (only cu12.6 wheels
  cover Pascal/Volta). The placement kernels are written directly in CUDA following the ePlace maths;
  PyTorch is used only offline for training learned models (doc 06 T3), with a cu12.6 wheel.
- **Code to study or port (BSD-3):** Xplace `cpp_to_py/gpugr` (GGR + GAMER sweep maze routing, pattern routing),
  InstantGR (batching, node-level parallelism), DREAMPlace/Xplace density kernels. OpenDRC (MIT) for GPU
  DRC structure; PDRC (DAC 2024) for non-Manhattan GPU DRC ideas.

## 6. Prior art

No published work implements a full GPU *detailed* router; GPU maze routers use sweeps/BFS/Bellman-Ford
rather than priority-queue A*, because A* parallelises poorly. This confirms decision D5: the GPU builds
fields and batches, the CPU runs the octilinear A* searches.


GAMER (GPU-accelerated maze routing, sweep with parallel scan), FastGR and GGR (GPU global routing with
pattern routing and batch scheduling), DREAMPlace and Xplace (GPU analytic placement with FFT density),
Karras 2012 (LBVH), Lee/Moore wavefront with bit-parallel BFS. OrthoRoute (MIT) shows GPU PathFinder in a
KiCad plugin, and its weak results on mixed boards are why TraceMaker keeps octilinear CPU detailed routing
as the quality path.

## 7. Implementation status (2026-10-02)

| Workload | Status | Notes |
|---|---|---|
| Cost-to-go fields (router A* heuristic) | **Done**: `src/gpu/field_cuda.cu` + CPU reference `field_cpu.cpp` | GAMER-style line sweeps (rows, columns, both diagonals, both directions) + via relaxation to a fixpoint; one thread per line; `cudaStreamPerThread` so the 8 portfolio routers share the two GPUs. Exact equality with the CPU reference tested on random grids on the P100 and V100; routed boards byte-identical with `--no-gpu`. Used for windows ≥ 60k lattice points; GPU ~2x faster than the CPU field. Gain on routing is modest today because routed copper and soft costs (not in the field) dominate the remaining search effort |
| Philox RNG fill | Done (toolchain test) | |
| Placement density / annealing | Not started | |
| DRC broad-phase | Not built (D55) | The broad-phase (uniform grid) takes 14 ms on the largest board; the DRC's time was in exact tests against zone fills, fixed on the CPU with an edge index: vme-wren 730 s → 7 s (doc 05 §19) |
| Global pattern and maze routing (GAMER / GGR port) | Not built (D56) | The CPU global router routes the tile graph in 0.02–0.5 s per board, and its plan does not improve routing (doc 05 §19) |

## 8. Native macOS / Metal (2026-10-06, D70)

**What was built**

| Part | What |
|---|---|
| Build | `macos-metal` and `macos-cpu` presets use Apple Clang; Metal uses Objective-C++20. CUDA and Metal are mutually exclusive; non-Apple builds retain the CUDA default |
| Device API | `compiled_backend`, `DeviceInfo::index`, and `field_gpu` select the compiled backend. CPU references are always built; unavailable devices and failed jobs fall back to CPU |
| `device_metal.mm` | Registry-ID-ordered discovery with `METAL-<registry-id>` identifiers. Memory admission uses `recommendedMaxWorkingSetSize - currentAllocatedSize` with a 256 MiB margin: process working-set headroom, not free system RAM |
| `field_metal.mm` | Embedded MSL, cached per-device pipelines, and per-thread shared buffers and command queues. Ordered integer line sweeps and via relaxation return only converged fields; buffer barriers order dispatches, and the CPU reads results after completion |
| Scope | Cost-to-go fields only. Philox remains CUDA-only; placement, A*, and exact legality checks stay on the CPU |

**Results.** On Apple M4 Pro, a private 4-layer sensor board (218 connections), seed 7, one variant, and
5,000,000 work units produced byte-identical CPU/Metal boards: 118/154 connections routed, 342 added tracks,
47 added vias, 73 Metal fields, zero fallbacks. The first kernel took 1.53 s for fields versus 0.88 s on CPU;
§8.1 replaces it.

KiCad 10.0.3 sign-off was **not clean**: 36 unconnected items and three added vias inside J5's footprint-local
keepout, on top of 17 input errors. Both backends reproduced this router/rule-coverage limitation. The input
board and project were unchanged.

There was no executable `quick` manifest. Regression used a seeded 30-board tier-A sample
(`bench/run.py --tier A --limit 30 --seed 1`), one variant, seed 7, no knowledge base, and 1,000,000 work units
per board. All 30 CPU/Metal outputs were byte-identical, with no added judged routing errors. Each backend
completed 13/30 cleanly; mean completion was 89.33%. These runs are not comparable to the README's longer
portfolio runs. Results: `bench/results/macos-{cpu,metal}-quick/`. The generated table's RC12 zero means
missing baseline data; the summary records null.

CTest (first kernel): Metal 117 passed / 14 skipped; CPU 114 passed / 17 skipped; zero failures in each
131-test suite. Skips covered unavailable fixtures, Docker-dependent KiCad parity, YAML tooling, and
unsupported GPU workloads. Native KiCad judged the private board and regression sample separately.
The migrated CUDA sources could not be compiled or run on macOS.

### 8.1 Parallel line scans (2026-10-06, D71)

**Before.** Fields used 50% of busy CPU time on the cleared private board (20M work, 8 variants):
2,311 fields, mean 262k lattice points × layers, 4 layers, median 8 rounds, maximum 15, about 21 ms each.
One thread per line left the first Metal kernel slower than CPU with concurrent callers.

**What was built.** GAMER's line recurrence is a segmented prefix-min:
`d'[i] = c·i + min_{j≤i}(d[j] − c·j)` within each open run. One 32-lane SIMD-group scans a line in 32-cell
blocks, carrying the minimum between blocks. The backward scan mirrors the forward scan; each lane rereads
only cells it wrote. Disjoint lines and layers share dispatches without changing `field_cpu`'s per-round
state. A round uses four line dispatches and one via dispatch. Command buffers batch 16 rounds initially,
then 8; rounds after convergence do no work. GPU field time is about 0.4–1.5 ms.

**Results.** Apple M4 Pro, macOS 26.4; CPU/Metal outputs were byte-identical.

| Measurement | CPU | Metal (first kernel) | Metal (scan kernel) |
|---|--:|--:|--:|
| 2,311 captured fields, one caller | 47.7 s | 41.7 s | 10.3 s, 0 mismatches |
| 8 concurrent callers × 2,311 fields | 54.1 s | 107.9 s | 18.2 s |
| Field time per rip-up variant | 8.3–12.0 s | 8.5–11.4 s | 1.0–1.5 s |
| Cleared private board, 20M work, 8 variants | 16.2 s (21.4 s wall) | 15.2 s | 9.2 s (11.8 s wall), 208/218 |
| `tracemaker-place --mode routable`, 17 routes | 336.0 s | — | 228.5 s |

**Rules caveat.** The table used a renamed board without its project, hence default rules and a 0.075 mm
lattice. With project rules (0.050 mm lattice), the route took about 121 s on either backend; field time per
variant was 1.6 s on Metal or 6.4 s on CPU. A* dominated. The default 120 s `--time` limit stopped the run
before 20M work, so non-best variants differed between runs; the output boards remained identical.
Placement's 600 s router safety limit gave 55–147 s per route, 987 s for 7 routes.

The same 30-board sample gave 30/30 byte-identical CPU/Metal boards, 43.3% clean pass, 89.3% completion, and
no added errors (`bench/results/macos-metal-scan-quick/`). The `[gpu][field]` tests cover thin grids, blocked
targets, buffer reuse, concurrent callers, and a 300-cell winding corridor requiring more than 32 rounds.
