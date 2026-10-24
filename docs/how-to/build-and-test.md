---
status: current
code: [CMakeLists.txt, CMakePresets.json, scripts/bootstrap.sh, tests]
---

# Build and test

## Build

```bash
cmake --preset release && cmake --build --preset release      # with CUDA 12.x
cmake --preset cpu-only && cmake --build --preset cpu-only    # without CUDA
```

The programs land in `build/<preset>/src/app/tracemaker`, `build/<preset>/src/place/tracemaker-place` and
`build/<preset>/src/server/tracemaker-view`.

| Preset | What it is |
|---|---|
| `release` | Optimised, CUDA kernels for `sm_60` and `sm_70` |
| `cpu-only` | Optimised, no CUDA. Results are identical to `release` |
| `debug` | No optimisation, CUDA |
| `asan`, `tsan` | AddressSanitizer + UBSan, ThreadSanitizer. Python bindings are off in these (D18) |
| `dist` | The release binaries: no CUDA, static libstdc++, SQLite and zstd, no tests (`scripts/make_release.py`) |
| `macos-metal`, `macos-cpu` | Apple silicon, with or without the Metal backend (contributed, not run by the project) |

`scripts/bootstrap.sh` configures, builds and tests `release`, `cpu-only` and `asan` in one go.

### Compilers

The presets do not fix the compilers. `CMakeLists.txt` takes the newest of `/usr/bin/g++-15`, `-14`, `-13` for C++
and `/usr/bin/g++-13` or `-12` for CUDA host code (`nvcc` 12.4 rejects GCC 14 and newer). To choose your own:

```bash
CXX=/path/to/g++ cmake --preset release                        # or -DCMAKE_CXX_COMPILER=...
TM_CUDA_HOST_CXX=/path/to/g++-13 cmake --preset release        # or -DCMAKE_CUDA_HOST_COMPILER=...
```

!!! warning "On the development machine"
    `g++` on the user's `PATH` is a Nix toolchain whose linker cannot see the system CUDA libraries. Do not export
    `CXX` to it; the presets put `/usr/bin` first on `PATH` for this reason.

Stay on CUDA 12.x: CUDA 13 cannot compile for the Pascal (P100) and Volta (V100) GPUs.

Warnings are errors (`-Wall -Wextra -Werror`), and the sanitizer presets must stay green.

## Test

```bash
ctest --preset release                 # 391 tests, about 20 minutes
ctest --preset release -R route        # only tests whose name matches
ctest --preset asan                    # memory errors; its integration tests are slow
```

- Tests that need a benchmark board, `kicad-cli` or a GPU **skip** (exit code 77) when it is missing; they never
  fail for that reason. Get the boards with `scripts/fetch_fixtures.sh` ([benchmarks](benchmarks.md)) and
  `kicad-cli` from Docker ([kicad-cli from Docker](kicad-cli-docker.md)).
- After the first build, run `scripts/fetch_fixtures.sh derived` once more to build the placement test set.
- Unit tests use Catch2 and live next to the code (`src/<module>/test_*.cpp`) or in `tests/`. Integration tests are
  scripts in `tests/integration/`, registered in `tests/CMakeLists.txt`.
- A test with a work budget must not depend on wall-clock time: pass `--time 3600` with `--work`, or a slow build
  stops early and fails for the wrong reason.
- Every accelerated path (GPU, spatial index) has a test that compares it with its reference path.

## Before you finish a change

- `ctest --preset release` passes.
- A change to routing or placement runs the `quick` benchmark tier and reports the result, good or bad
  ([benchmarks](benchmarks.md#the-quick-tier)).
- A change to the design updates its doc and adds a [decision record](docs.md#record-a-decision) in the same change.
