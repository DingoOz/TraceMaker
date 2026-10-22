---
status: current
code: [src/gpu]
---

# Use the GPUs

TraceMaker runs without a GPU and gives the same results with one: the GPU computes cost-to-go fields (and, in the
placer, the density and annealing work) faster, but never decides what is legal (decision
[D5](../decisions/D005.md)). Every kernel has a CPU reference and a test that both agree bit for bit
([GPU acceleration](../07-gpu.md)).

## Check what it sees

```bash
build/release/src/app/tracemaker gpu-info
```

It lists each device with its name, UUID and the memory it may use. A `cpu-only` build prints
`This build has no GPU support (cpu-only). CPU reference paths are used.`

## Choose devices

The router uses the GPU when the build has CUDA and a device has memory free. `--no-gpu` computes everything on the
CPU. The placer is the other way round: its `--gpu` option turns the GPU on.

Choose devices **by UUID, never by index**: CUDA orders the fastest device first (the V100 is device 0 on the
development machine), while `nvidia-smi` lists the P100 first.

```bash
nvidia-smi -L                                         # names and UUIDs
CUDA_VISIBLE_DEVICES=GPU-xxxxxxxx-... build/release/src/app/tracemaker route board.kicad_pcb -o out.kicad_pcb
```

## Shared GPUs

The development machine's GPUs are shared with other jobs (a `llama-server` has held nearly all their memory).
TraceMaker treats GPU memory as scarce: a failed allocation or kernel falls back to the CPU with the same result, and
the route summary counts the fallbacks (`fields 0 GPU + 34 CPU ... 0 GPU fallbacks`). Tests must pass with no GPU
available; do not stop other people's jobs to free memory.

## Building for CUDA

CUDA 12.x only (CUDA 13 dropped Pascal and Volta). `nvcc` 12.4 rejects GCC 14 and 15 as host compiler, so CUDA host
code is built with `g++-13` or `g++-12` while the rest uses GCC 15 ([Build and test](build-and-test.md#compilers)).
