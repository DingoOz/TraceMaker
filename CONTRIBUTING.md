# Contributing to TraceMaker

Thank you for wanting to help. This page tells you how to get a change accepted with the least back and forth.

## In one minute

- **Small, focused pull requests** are reviewed fastest. One idea per pull request.
- **Just send the pull request.** There is no need to ask first. Most contributors work with an AI assistant, and
  this page assumes you do too.
- **Every change must keep the tests green** (`ctest --preset release`).
- **Changes to routing or placement need a benchmark run**, and the numbers go in the pull request, good or bad.
- **Never commit benchmark boards.** They are downloaded, not redistributed.
- Your contribution is licensed under **GPL-3.0-or-later**, like the rest of the project.

## Ways to help

- **Report a bug.** A board that routes wrongly, or a result that KiCad's design-rule check rejects, is the most
  useful report there is.
- **Fix a bug** from the [issue list](https://github.com/DingoOz/TraceMaker/issues).
- **Improve the documentation**, including this page.
- **Try it on your own boards** and say what happened.
- **Add a feature.** Send the pull request; say what it is for and what you measured.

## Working with an AI assistant

That is the expected way to contribute here. A few things make it go well.

- **Point the assistant at [CLAUDE.md](CLAUDE.md) first.** It holds the build commands, the machine notes and the
  rules below, written for an assistant to follow. Then the design document for the area you are changing
  (`docs/11-roadmap.md` lists them).
- **You are the author.** Read the change before you send it, and be ready to answer questions about it.
- **Numbers must be measured, not predicted.** If the pull request says "routes 12 more connections", there must
  be a run that shows it. Have the assistant paste the command and the result.
- **Have it run the tests and the quick tier**, not just say the change should pass.
- **Ask it to state what it did not check.** An honest "not tested on tier B" is worth more than a confident
  summary.
- **Keep the change small.** Assistants will happily rewrite more than you asked for; unrelated edits get the pull
  request sent back.

## Reporting a bug

Open an [issue](https://github.com/DingoOz/TraceMaker/issues) with:

1. **What you ran:** the full command line.
2. **What happened** and **what you expected.**
3. **The board**, if you are able to share it, or the smallest board that shows the problem.
4. **The versions:** `tracemaker version`, your KiCad version, and whether a GPU was used.
5. **For a wrong result:** the output of `kicad-cli pcb drc --format json` on the routed board.

Only attach a board if you have the right to share it publicly.

## Setting up

```
cmake --preset release && cmake --build --preset release
scripts/fetch_fixtures.sh          # test boards, about 2.4 GB, once
ctest --preset release             # 132 tests, about 5 minutes
```

- No CUDA? Use the `cpu-only` preset.
- Tests that need a board or `kicad-cli` skip when it is missing. `scripts/kicad-cli` runs KiCad's command line
  from Docker.
- More detail is in the [README](README.md).

## Before you open a pull request

A checklist. Tick what applies to your change.

- [ ] It builds without warnings (warnings are errors here).
- [ ] `ctest --preset release` passes.
- [ ] New behaviour has a test. A bug fix has a test that failed before the fix.
- [ ] If it touches routing or placement: the quick benchmark tier was run (see below) and the result is in the
      pull request.
- [ ] If the design changed: the matching file in `docs/` is updated in the same pull request, with a row in
      [docs/12-decisions.md](docs/12-decisions.md).
- [ ] No benchmark boards, build output or personal paths are in the commit.

### The quick benchmark tier

```
python3 bench/run.py --tier A --limit 40 --time 120 --threads 8 --jobs 4 --name my-change-tierA
```

It takes about eight minutes. It must stay at 40 of 40 boards clean with no error added by routing.

For a change meant to improve results, also compare it with the unchanged program **on the same machine at the same
time**, and say what you measured. A change that does not help is still worth reporting; it is not worth merging.

## Rules the code must follow

These are not style preferences. A change that breaks one will not be merged.

1. **Correctness before speed.** Nothing is added to the board without passing the exact geometric check.
2. **Determinism.** Seeded random numbers only. Stable tie-breaks. Never loop over a hash container where the order
   can affect the output. Costs are integers.
3. **Every fast path has a plain reference path** (CPU for GPU, a linear scan for an index) and a test showing both
   give the same result.
4. **Changes to the board go through transactions** and are kept only if they improve it.
5. **Budgets inside loops are counted in work units**, not seconds. Wall-clock time only stops the whole job.
6. **Locked items are never moved or ripped up.** If a rule could not be read, warn and be conservative.
7. **Emit events** for anything a user would want to watch. Never wait for the viewer.
8. **KiCad files:** content that was not changed is written back byte for byte.
9. **No regressions:** run the quick tier before finishing a routing or placement change, and report the result
   honestly.

## Code style

- **C++20.** Functions and variables in `snake_case`, types in `PascalCase`.
- **One namespace per module** under `tmk` (`tmk::geom`, `tmk::route`, …). Not `tm`, which clashes with C.
- **Units:** `int64` nanometres inside the engine (`tmk::Coord`). Millimetres only where files or people are
  involved.
- **Small files, clear names.** Comments explain *why*, not what.
- **Cite the paper** where an algorithm from the literature is defined.
- **Every source file starts with** `// SPDX-License-Identifier: GPL-3.0-or-later` (or `#` for scripts).
- Match the code around you: its naming, its comment density, its idioms.

## Tests

- **Unit tests** live next to the code (`src/<module>/test_*.cpp`) or in `tests/`. They use Catch2.
- **Integration tests** are scripts in `tests/integration/`, registered in `tests/CMakeLists.txt`. A test that
  needs a board or KiCad must exit with code 77 when it is missing, so it is reported as skipped.
- **A test with a work budget must not depend on wall-clock time.** Pass `--time 3600` with `--work`, or a slow
  build will stop early and fail for the wrong reason.
- **Sanitizers:** `ctest --preset asan` should report no memory error. Its integration tests are slow.

## Benchmark discipline

- **KiCad is the judge.** A result counts when `kicad-cli pcb drc` says so.
- **Do not tune on held-out boards.** The lists in `bench/place_sets/` are frozen. A new held-out set gets a new
  name.
- **Compare like with like:** the same binary, two settings, run side by side. Machine load changes timed results.
- **Report losses as plainly as gains.**

## Commits and pull requests

- **Commit messages:** a short first line saying what changed, then why, and what you measured if anything.
- **Keep unrelated changes apart.** A fix and a refactor are two pull requests.
- **Describe the pull request** in a few sentences: the problem, the change, how you tested it, and what you did
  not test.
- Expect questions. Review is about the change, not about you.

## Licence of contributions

TraceMaker is free software under the GNU General Public License, version 3 or later, with a permission to link
with NVIDIA's CUDA runtime (see [LICENSE](LICENSE) and [NOTICE](NOTICE)).

By submitting a contribution you agree that it is licensed under those same terms, and that you have the right to
submit it. If you bring in code from elsewhere, say where it came from and under which licence, and add it to
`NOTICE`.

## Questions

Open an [issue](https://github.com/DingoOz/TraceMaker/issues). There is no question too small.
