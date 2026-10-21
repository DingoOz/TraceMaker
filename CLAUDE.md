# TraceMaker — guidance for implementation sessions

Read `PLAN.md`, then the docs for the milestone you are working on (`docs/11-roadmap.md` lists them).
The design docs are the source of truth. If the code needs to differ, update the doc in the same change
and add a decision record: `scripts/decisions.py new "Short title" --docs NN` writes `docs/decisions/DNNN.md` and
updates the index in `docs/12-decisions.md` (never add a table row there; D134).

## Machine

- 56 cores, ~150 GB RAM, Tesla P100 (`sm_60`, 12 GB) and V100 (`sm_70`, 16 GB), CUDA 12.4, GCC 15, no clang.
- CUDA 13 cannot compile for these GPUs: stay on CUDA 12.x. `nvcc 12.4` rejects GCC 15; use
  `CMAKE_CUDA_HOST_COMPILER=g++-12` (builds for both; ran correctly on the P100, the V100 had no free memory at the time).
- The GPUs are shared with other jobs (e.g. `llama-server` may hold nearly all GPU memory). Never assume free
  GPU memory; tests must pass with the GPU unavailable. Select devices by name/UUID: CUDA orders the V100
  first, `nvidia-smi` lists the P100 first.
- KiCad is not installed natively. Use `kicad-cli` from Docker `kicad/kicad:10.0.6` (wrapper script in
  `scripts/`), or install KiCad 10 if allowed.

## Build and test

```
scripts/bootstrap.sh                 # configure + build + test presets release, cpu-only, asan
cmake --preset release && cmake --build --preset release && ctest --preset release
build/release/src/app/tracemaker gpu-info
python3 bench/check_env.py           # kicad-cli, engine binary, Freerouting, fixtures
scripts/fetch_fixtures.sh [set ...]  # freerouting dac2020 kicad-demos derived (default, pinned commits), pcbench, pcbworld
```

- `CMakeLists.txt` (not the presets) defaults to `/usr/bin/g++-15` for C++ and `/usr/bin/g++-13` for CUDA host
  code, overridable with `CXX` / `-DCMAKE_CXX_COMPILER` and `TM_CUDA_HOST_CXX` / `-DCMAKE_CUDA_HOST_COMPILER`; the
  presets put `/usr/bin` first on PATH. `g++` on the user's PATH is a Nix toolchain whose linker cannot see the
  system CUDA libraries, so do not export `CXX` to it.
- The C++ namespace is `tmk` (`tm` clashes with C's `struct tm`).
- `bench/results/` is git-ignored. Results of a milestone's tests and experiments are copied into the repo before the work is called
  done: `bench/records/<milestone>/` (raw result files, the advice/option files used, test outputs, a README with the tables) and a
  summary in the design doc (see `bench/records/m16/`).
- Fixtures live in `bench/data/` (git-ignored). Freerouting's PCBench set (1,158 boards with `.kicad_pcb` and
  `.dsn`) is at `bench/data/freerouting/scripts/benchmark/fixtures/PCBench`.

## Documentation site

- MkDocs + Material from `docs/`, organised by Diátaxis (tutorials, how-to, reference, explanation, project); see
  `docs/how-to/docs.md`. Preview: `pip install -r docs/requirements.txt`, `mkdocs serve`; CI runs `mkdocs build --strict`.
- The numbered design docs keep their names (`docs/05-routing.md`, "doc 05 §34"); new pages go in `nav` in `mkdocs.yml`.
- Every page starts with a YAML header: `status` (current | partly-built | plan | historical), `code` (paths it
  describes), optional `reviewed` (date checked against the code). `scripts/docs_check.py` checks them.
- Generated, committed pages: `scripts/gen_reference.py cli --build build/release` after changing an option or its help,
  `scripts/gen_reference.py bench` after adding to `bench/records/`. CI fails when they are out of date.
- C++ API: Doxygen reads the headers' plain `//` comments through `scripts/doxygen_filter.py`; keep writing `//`
  comments above declarations. New undocumented declarations raise a CI warning (`scripts/check_api_docs.py`).

## Progress website

- A read-only dashboard runs at `http://<host>:8765/` (systemd user service `tracemaker-devsite`, bound to
  0.0.0.0). It reads `dev/progress.json`, `dev/activity.jsonl`, `build/*/test-results/*.xml`, git, nvidia-smi and
  `bench/results/*/summary.json`.
- Keep it truthful: tick tasks and set milestone status in `dev/progress.json` as work lands, and log notable
  events with `scripts/devlog.py --kind build|test|note|decision|milestone "text"`.
- Benchmark runs should write `bench/results/<run-id>/summary.json` with `set`, `boards`, `clean_pass`,
  `completion` and `seconds` so they appear on the site.

## Non-negotiable rules

1. **Correctness before speed.** Nothing may be committed to the board without the exact geometric check.
2. **Determinism.** Seeded counter-based RNG; stable tie-breaks; never iterate a hash container in an
   output-affecting order; integer fixed-point costs; GPU reductions order-independent.
3. **Every accelerated path has a reference path** (CPU for GPU, linear scan for an index) and a test that
   both give identical results.
4. **Transactions only.** Stages modify the board through branches; commit only if the score improves.
5. **Budgets by work units** inside loops; wall time only stops the job.
6. **Never move or rip locked items**; never route on rules that could not be read (warn, be conservative).
7. **Emit events** for everything a user would want to see; never block on the viewer.
8. **KiCad files**: untouched content round-trips byte-for-byte.
9. **No regressions**: run the `quick` benchmark tier before finishing a change that touches routing or
   placement; report results honestly, including failures.

## Style

- C++20, `snake_case` functions and variables, `PascalCase` types, one namespace per module under `tmk` (not `tm`, which clashes with C's `struct tm`)
  (`tmk::geom`, `tmk::route`, …). Headers in `include/`-less layout next to sources (`src/<module>/`).
- `-Wall -Wextra -Werror`; sanitizer presets must stay green.
- Units: `int64` nanometres in the engine (`tmk::Coord`); millimetres only at I/O and UI boundaries.
- Small files, clear names, comments that explain *why*. Cite the paper for each algorithm at its definition.
