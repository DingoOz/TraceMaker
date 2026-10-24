TraceMaker 0.11.0 adds a run report a designer can audit, a documentation site, and a contributed macOS 15 build. The router's search is the same as 0.10.0's; copper differs only where the shapes measured against KiCad (D132) changed an obstacle.

## Install without building (Linux x86_64)

- `tracemaker-0.11.0-linux-x86_64.tar.gz`: the router, the placer, the live viewer, an example board and the agent skills. Start with its `INSTALL.txt`.
- `tracemaker-0.11.0-pcm-linux-x86_64.zip`: the KiCad 10 plugin with the router inside. KiCad > Plugin and Content Manager > Install from File...

They need glibc 2.38 or newer (Ubuntu 24.04, Debian 13, current Fedora; Debian 12 and Ubuntu 22.04 are too old), no CUDA and no other library. CI installs them in a clean container with no compiler and routes the example board (D121).

## New

- **Run report** (M20 D3, D136, [how to read it](https://dingooz.github.io/TraceMaker/latest/how-to/run-report/)): `route --report report.html` (or `.md`) writes one file that answers what was routed, what is open and why (by cause, with the escape and cut evidence), which rules were read and which were not, the signal-integrity audit, TraceMaker's DRC of the board before and after the route with zones refilled, and the options and seed. The report is rendered from the route summary alone: every number in it is a field of the JSON, and the HTML shows the JSON pointer of any value. `tracemaker report summary.json -o report.html` renders it again from a saved summary.
- **Every route summary** (`--json`) now carries `open`, `completion_pct`, `time_limited` and `run` (version, paths, date, seed, budgets, variants, threads and the options as given). `time_limited` says when the wall-clock limit, not the work budget, ended the search: such a result depends on machine speed and load, and the route log warns about it.
- **Documentation site** (D134): MkDocs and Material, organised as tutorials, how-to guides, reference and explanation, with decision records, generated CLI and benchmark reference pages, and the C++ API through Doxygen. A tag publishes a versioned copy. Quick start for Linux (download or build), macOS and Windows (WSL2).
- **macOS 15 and 26 build with a Metal backend**, contributed by Chris Hemingway (#46, D135). Tested on contributors' Macs, not in CI; no macOS release file yet.
- **Judge and DRC shapes measured against KiCad** (D132): copper text boxes, custom-pad primitives, margin-layer graphics and negative solder-mask margins now match KiCad's measurements, and the judge no longer counts a net-less copper arc as routing.
- **Viewer framing** (D133): `--view-embed "<origins>"` lets named pages embed a viewer; the default still forbids framing and the token still guards every request.

## Results

PCBench, 8 router variants, 120 s per board, judged by KiCad's own DRC (`kicad-cli pcb drc`, the D106 judge, the same as 0.10.0). No board has an error added by routing.

| Tier | Boards | 0.11.0 clean | 0.10.0 clean | Freerouting 2.5.0-RC12 (published) | 0.11.0 completion |
|---|--:|--:|--:|--:|--:|
| A | 40 | **100%** | 100% | 100% | 100% |
| B | 40 | **70.0%** | 70.0% | 50.0% | 97.1% |
| C | 30 | **46.7%** | 46.7% | 46.7% | 97.4% |
| D | 22 | **36.4%** | 40.9% | 36.4% | 94.9% |

Tier D is one board lower: Keyboard_PCB_Keyboard routed 530 of its 534 connections. The machine was shared with other jobs (load 40 to 78 on 56 cores) and the limit is wall-clock time, so the 0.10.0 binary was run beside it under the same load: it scored 31.8% (7 boards, and 528 of 534 on that keyboard). The difference is load, not the router. With its options off, the run report changes no copper: the quick tier at a fixed work budget is byte-identical with and without it on all 30 boards.

Records: `bench/records/release-0.11.0/` (every run's summary and per-board results, the comparisons, the test log).

Test suite: 391 tests, all passing in the release build (one skips without its fixture; the two GPU tests skipped in the full run and passed when run again with the GPUs idle).

## Known limits

- The gates of 0.8.0 are still not met: from-scratch placement (85% against 90%) and dense-package boards (41% clean).
- The agent interface and the advisor are experimental (neither beat the plain router at equal compute).
- On the plane board `interf_u` with `--soft-zones --si-rules`, a run stays on one core for more than 20 minutes after its variants finish (27 s in earlier records); not yet explained. It predates this release.
- The KiCad plugin and the Python module do not pass `--report` yet; the report's DRC is TraceMaker's own (KiCad's violation names), so sign off with `kicad-cli pcb drc`.
- The release files are Linux x86_64 only.
