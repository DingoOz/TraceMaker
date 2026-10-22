# macOS 15 records: building and testing on macOS before 26

Design: [docs/07-gpu.md](../../../docs/07-gpu.md) section 8.2, decision D135. Machine: Apple M2, 8 cores, 16 GB, macOS 15.7.9,
Apple clang 17.0.0, CMake presets `macos-metal` and `macos-cpu`, native `kicad-cli` 10.0.6 (Homebrew cask `kicad`), KiCad demo
fixtures (KiCad 10.0 at 7a7d026) and Freerouting's PCBench boards from `scripts/fetch_fixtures.sh`.

## What was wrong and what changed

| Problem on macOS 15 | Change |
|---|---|
| `std::from_chars` for `double` is unavailable before macOS 26 in Apple's libc++: `sexpr.cpp`, `project_reader.cpp`, `layer_limits.cpp` did not compile | `tmk::parse_double` (`src/core/parse_double.hpp`): `from_chars` off Apple, `strtod_l` in the "C" locale on Apple, held to `from_chars`' grammar |
| `route/legalize.cpp`: an unused constant, an error under clang's `-Wunused-const-variable` with `-Werror` | removed |
| `drc_parity.sh`, `drc_broken_parity.sh`: `mapfile` does not exist in bash 3.2 | a `while read` loop |
| `drc_zone_index.sh` (and the two above): an empty array is "unbound" under `set -u` in bash 3.2 | `${a[@]+"${a[@]}"}` |

`std::to_chars` for `double` (used by `board_editor.cpp`) is available from macOS 13.3 and needed nothing.

## What was checked (`verification/`)

- `ctest-macos-metal.txt`, `ctest-macos-cpu.txt`: the full suite on each preset with the fixtures present, so the KiCad tests ran
  instead of skipping: DRC parity against `kicad-cli` (14/14 boards), broken-board parity, `drc_zone_index` (index equals the linear
  reference on 4 boards, stored and refilled). 368/368 pass on each. Skipped on both: the CUDA-only Philox test, `kicad_edit_roundtrip`
  (needs the KiCad Docker image), `python_bindings` (pybind11 not installed, so the module was not built), `crules_catalogue_sync`
  (PyYAML not installed); on `macos-cpu` also the four GPU field tests (no GPU backend in that build).
- `sanitizers.txt`: the full suite built with the `asan` preset's flags (`-fsanitize=address,undefined -fno-sanitize-recover=all`,
  Debug) on Apple clang, `macos-cpu` otherwise, `ctest -j4`: no ASan or UBSan report; 367/368 pass. `crules_two_stage` (a placement
  run, no code this change touches) passed its checks up to stage 2 and then hit its 3,600 s limit at 4,360 s, with three other tests
  on 8 cores; it passes in both release suites. `asan-crules_two_stage.txt`: the same script run alone, outside ctest, under the same ASan build:
  passed (exit 0, no sanitizer report) in 6,389 s wall and 1,104 s CPU. The test's 3,600 s limit is too short for an ASan Debug
  build on this machine; it is left as it is, since the Linux `asan` preset is the one the project runs.
- `parse-double-xcheck.txt`: `parse_double_xcheck.cpp` compiled with Homebrew GCC 16, so the macOS `strtod_l` path runs against
  libstdc++'s `from_chars`: 2,000,000 strings (random number-like characters, `%.17g`/`%f`/`%g` renderings of random doubles, values
  near the subnormal range), 0 mismatches in what is accepted and in the double's bits. The first version rejected subnormals
  (`strtod` sets `ERANGE` for them, `from_chars` returns them): found by this check, fixed, and covered by `tests/test_units.cpp`, which
  also compares the two paths bit for bit on Linux.
- Quick tier, `bench/run.py --set quick --work 1000000 --jobs 4` (2 router threads per board), with each binary (`runs/`):

| Run | Boards | Clean | Completion | Added errors | Router s | CPU s |
|---|--:|--:|--:|--:|--:|--:|
| `macos-quick-cpu` | 30 | 25 (83.3 %) | 99.0 % | 0 | 197.6 | 214.8 |
| `macos-quick-metal` | 30 | 25 (83.3 %) | 99.0 % | 0 | 140.6 | 123.5 |

  - `quick-cpu-vs-metal.md`: `compare_runs.py --expect-identical`: routed boards byte-identical 30/30, gates pass.
  - `quick-cpu-vs-linux-m17.txt`, `quick-metal-vs-linux-m17.txt`: `compare_boards.py` against `bench/records/m17/runs/m17-quick-new`
    (Linux, `--no-gpu`, 8 threads, the last Linux quick run, byte-identical to M18's): 30/30 boards with the same routed connections,
    tracks, vias, completion, clean result, added errors and style metric. The routed Linux files are not in the repository, so
    this compares fingerprints, not bytes.

There was no earlier macOS 15 run to compare with (the engine did not build there). The quick-tier binaries were built from 33e0e04 (this
branch's code on top of main at 54e551f); what was committed after it changes comments, test scripts and docs only.
Wall and CPU seconds are from a shared laptop and are not a speed claim.
