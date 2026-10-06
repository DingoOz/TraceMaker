# TraceMaker

A placement-aware PCB autorouter for KiCad, written in C++20 with optional CUDA or Metal. It reads KiCad 9 and 10 boards and
projects directly, routes with negotiated rip-up and reroute on an exact-geometry lattice, learns from failed
attempts within and across runs, writes the result back into the `.kicad_pcb` without disturbing anything else,
and is judged by KiCad's own DRC. A browser viewer shows routing live.

Design: [PLAN.md](PLAN.md) and [docs/](docs/). Decisions taken without the user's input: [dev/assumptions.md](dev/assumptions.md).

## Build

### macOS (Apple silicon)

Install Apple's Command Line Tools or Xcode, then the build dependencies:

```sh
brew install cmake ninja eigen cli11 nlohmann-json catch2 zstd boost
# Optional: brew install pybind11 python ccache
cmake --preset macos-metal -DCMAKE_PREFIX_PATH="$(brew --prefix)"
cmake --build --preset macos-metal -j 8
ctest --preset macos-metal
build/macos-metal/src/app/tracemaker gpu-info
build/macos-metal/src/app/tracemaker route board.kicad_pcb -o routed.kicad_pcb --work 5000000 --no-kb
```

Use `macos-cpu` for a CPU-only build, or `--no-gpu` on the Metal executable. Metal accelerates cost-to-go
fields; A* and exact legality checks stay on the CPU. Kernels are embedded, so no runtime shader files are
needed. `gpu-info` reports working-set headroom, not system-wide free memory. See
[docs/07-gpu.md](docs/07-gpu.md) for scope and verification.

Install KiCad separately and put `kicad-cli` on `PATH` for external DRC. The route command does not run that
check automatically. Build the browser viewer with the `viewer/` command below.

### Linux (CUDA or CPU)

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

Placement (opt-in; keeps the input placement unless the new one routes at least as well):

```
build/release/src/place/tracemaker-place board.kicad_pcb -o placed.kicad_pcb --mode auto --route-check 3000000
```

Escape feasibility and component-aware rules (both report-only unless asked for):

```
build/release/src/app/tracemaker escape board.kicad_pcb            # dense-package pins that cannot escape, and why
build/release/src/app/tracemaker rules board.kicad_pcb --mode on --dru rules.kicad_dru   # detected parts, rules, impedance
build/release/src/app/tracemaker route board.kicad_pcb -o out.kicad_pcb --component-rules on   # + crystal/inductor keep-outs
build/release/src/place/tracemaker-place board.kicad_pcb -o placed.kicad_pcb --mode full --component-rules soft
```

`--rules-override overrides.json` corrects detections and rules (doc 15 §6.3). Escape planning (doc 05 §12) runs in
two of the eight portfolio variants; `--escape-plan` turns it on in all of them.

KiCad 10 plugin: `kicad_plugin/` (IPC action plugin; routes the open board in one undoable commit; see its README).

Useful route options: `--threads N` (threads; also the portfolio size unless `--work` is given, when all eight variants
run and the output is identical at any thread count), `--variants N` (portfolio size), `--no-gpu` (CPU cost-to-go fields, identical results),
`--no-rip-up`, `--fast-bends`, `--kb FILE` / `--no-kb` (knowledge base of earlier runs).

## Benchmark

```
scripts/fetch_fixtures.sh                     # Freerouting fixtures incl. PCBench, DAC2020, KiCad demos
python3 bench/run.py --tier B --limit 40 --time 120 --jobs 2 --threads 8
```

Latest results (PCBench, 120 s per board, judged by `kicad-cli`; Freerouting figures are its published results):

| Tier | Boards | TraceMaker clean | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 |
|---|--:|--:|--:|--:|
| A | 40 | 100% | 100% | 87.5% |
| B | 40 | 67.5% | 50.0% | 12.5% |
| C | 30 | 60.0% | 46.7% | 16.7% |
| D | 22 | 50.0% | 36.4% | 0.0% |

Held-out quality benchmark (`bench/quality_bench.py`): 60 boards never used in development, Freerouting 2.5.0-RC12 and
1.9.0 run on the same machine, everything judged by KiCad's DRC. KiCad-clean: TraceMaker 73–78%, Freerouting 2.5 3%,
Freerouting 1.9 32%. Against Freerouting 2.5 TraceMaker's tracks are 5% shorter with 28% fewer bends but 43% more vias.
DAC 2020 (10 boards, same judge): TraceMaker clean 60% (as many as the human-routed originals), Freerouting 2.5 10%,
Freerouting 1.9 20%. Details and figures: `report/report.pdf`.

Each run writes `bench/results/<run>/` (routed boards, per-board JSON, `summary.json`, `report.md`) and compares
against Freerouting's own published per-board results on the same fixtures. Results appear on the progress
site (`http://<host>:8765/`).

## Layout

`src/core` units, RNG, events · `src/sexpr` lossless s-expressions · `src/io/kicad` board/project/netlist I/O and
editor · `src/model` board and rules · `src/geom` exact geometry · `src/drc` KiCad-equivalent DRC and
connectivity · `src/route` router (obstacles, A*, negotiation, portfolio) · `src/learn` knowledge base ·
`src/gpu` CUDA/Metal fields with CPU references · `src/server` viewer server · `src/place` placement ·
`viewer/` WebGL2 viewer · `bench/` harness · `devsite/` progress website · `tests/` tests.

## Licence

TraceMaker is free software under the GNU General Public License, version 3 or (at your option) any later version
(`GPL-3.0-or-later`); see [LICENSE](LICENSE). [NOTICE](NOTICE) adds a section 7 permission to link with NVIDIA's
CUDA runtime, lists third-party components, and credits the KiCad demo projects behind `tests/truth/`. Benchmark
boards are downloaded, not redistributed, and keep their own licences.
