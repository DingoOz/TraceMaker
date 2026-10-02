# TraceMaker

A placement-aware PCB autorouter for KiCad, written in C++20 with CUDA. It reads KiCad 9 and 10 boards and
projects directly, routes with negotiated rip-up and reroute on an exact-geometry lattice, learns from failed
attempts within and across runs, writes the result back into the `.kicad_pcb` without disturbing anything else,
and is judged by KiCad's own DRC. A browser viewer shows routing live.

Design: [PLAN.md](PLAN.md) and [docs/](docs/). Decisions taken without the user's input: [dev/assumptions.md](dev/assumptions.md).

## Build

Ubuntu 26.04 with the packages from the setup script (`g++-13` for CUDA host code, Boost, Eigen, oneTBB, fmt,
spdlog, FlatBuffers, SQLite, Catch2, pybind11, Docker for `kicad-cli`).

```
cmake --preset release && cmake --build --preset release
ctest --preset release            # unit + integration tests (KiCad tests use the kicad/kicad:10.0.6 image)
cd viewer && npm install && npm run build   # browser viewer (served by the engine)
```

Presets: `release`, `debug`, `cpu-only` (no CUDA), `asan`, `tsan`.

## Use

```
build/release/src/app/tracemaker route board.kicad_pcb -o routed.kicad_pcb --time 120     # 8-variant portfolio
build/release/src/app/tracemaker route board.kicad_pcb -o routed.kicad_pcb --view --hold  # live view on :8766
build/release/src/app/tracemaker route board.kicad_pcb -o routed.kicad_pcb --work 50000000 # deterministic budget
build/release/src/app/tracemaker drc board.kicad_pcb --json report.json                   # KiCad-equivalent DRC
build/release/src/app/tracemaker inspect board.kicad_pcb                                   # board summary
build/release/src/server/tracemaker-view board.kicad_pcb --demo                            # viewer demo
kicad-cli pcb drc --format json -o drc.json routed.kicad_pcb                               # the judge
```

Useful route options: `--threads N` (portfolio size), `--no-gpu` (CPU cost-to-go fields, identical results),
`--no-rip-up`, `--fast-bends`, `--kb FILE` / `--no-kb` (knowledge base of earlier runs).

## Benchmark

```
scripts/fetch_fixtures.sh                     # Freerouting fixtures incl. PCBench, DAC2020, KiCad demos
python3 bench/run.py --tier B --limit 40 --time 120 --jobs 2 --threads 8
```

Each run writes `bench/results/<run>/` (routed boards, per-board JSON, `summary.json`, `report.md`) and compares
against Freerouting's own published per-board results on the same fixtures. Results appear on the progress
site (`http://<host>:8765/`).

## Layout

`src/core` units, RNG, events · `src/sexpr` lossless s-expressions · `src/io/kicad` board/project/netlist I/O and
editor · `src/model` board and rules · `src/geom` exact geometry · `src/drc` KiCad-equivalent DRC and
connectivity · `src/route` router (obstacles, A*, negotiation, portfolio) · `src/learn` knowledge base ·
`src/gpu` CUDA kernels with CPU references · `src/server` viewer server · `src/place` placement ·
`viewer/` WebGL2 viewer · `bench/` harness · `devsite/` progress website · `tests/` tests.
