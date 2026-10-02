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
| Placement density / annealing, DRC broad-phase, global maze routing | Not started | Profiling shows the A* loop itself (74%) is the router's bottleneck, not obstacle evaluation |
