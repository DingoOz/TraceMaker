# M16 records: advice files and a pluggable advisor (issue #26)

Results of the tests and experiments behind [docs/17-llm-advisor.md](../../../docs/17-llm-advisor.md) section 8, kept in the repository because
`bench/results/` is git-ignored. Story and pitfalls: [dev/m16-notes.md](../../../dev/m16-notes.md).

- Base: `origin/main` `49fbd3c` (PRs #27 to #30 merged), branch `m16-advisor`, 2026-10-14. Machine: 56 cores, no GPU used by the router
  (`--work` budgets, so a result does not depend on the machine or the thread count).
- Build for the comparisons with `main`: CPU-only preset, both sides, same machine.

## 1. Does advice help? (`advice-eval/`)

Board set `bench/sets/srj20-m15fail4.txt` (16 `srj20` boards of fewer than 70 connections still open after the 70 M portfolio run), split by
index: even = tuning half (8 boards, 489 connections), odd = held-out half (8 boards, 492 connections). Every run:
`bench/advice_eval.py --set srj20-m15fail4 --half even|odd --work 20000000 --variants 8 --threads 8 --advisor "python3 advisors/heuristic_advisor.py --strategy S"`
(control: `--control --advice-variants 4`, a plain route with 12 variants). Columns are connections routed; no board completes in any arm.

| Run (`advice-eval/<run>.json`) | Arm |
|---|---|
| `adv-a1` / `adv-b1` | advice `mixed`, first design (advised copies of variants 0, 2, 4, 6), even / odd half |
| `adv2-a1` / `adv2-b1` | advice `mixed`, final design (advised variants are the next four of the list), even / odd half |
| `adv-a2` | `stuck-first` (even half) |
| `adv-a3` | `corridor`, half width 0.35 mm (even half) |
| `adv-a4` | `corridor --keep-vias --half-width 0.5` (even half) |
| `adv-a5` | `stuck-first --layer-cost F.Cu=1.4 --priority-pairs 12` (even half) |
| `adv-c1` / `adv-bc` | equal-compute control: plain route, 12 variants, even / odd half |

The advice each run used is in `advice-eval/advice-files/<run>/<board>.advice.json` (replay with `route --advice FILE`).
The `plain-8` column is the plain run inside each advised experiment (the first run of `--advisor`): it is identical across experiments on the
same half (430 and 439), which is the check that the runs were work-budgeted and not cut short by a wall limit.

Even half (tuning). `stuck-first`, `corridor`, `corridor-vias-wide` and `stuck+cost+prio` were run with the first design (advised copies); only `mixed` was
run with both designs:

| board | connections | plain-8 | control-12 | mixed-copies | mixed-next | stuck-first | corridor | corridor-vias-wide | stuck+cost+prio |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| sample031 | 65 | 57 | 57 | 57 | 57 | 57 | 57 | 57 | 57 |
| sample054 | 61 | 56 | 55 | 56 | 56 | 56 | 56 | 56 | 56 |
| sample094 | 65 | 54 | 56 | 55 | 55 | 54 | 55 | 55 | 54 |
| sample100 | 52 | 47 | 47 | 47 | 47 | 47 | 47 | 47 | 47 |
| sample117 | 61 | 53 | 53 | 53 | 53 | 53 | 53 | 53 | 53 |
| sample119 | 58 | 54 | 55 | 54 | 54 | 54 | 54 | 54 | 54 |
| sample142 | 69 | 57 | 56 | 57 | 57 | 57 | 57 | 57 | 57 |
| sample174 | 58 | 52 | 52 | 52 | 52 | 52 | 52 | 52 | 52 |
| **total** | 489 | **430** | **431** | **431** | **431** | **430** | **431** | **431** | **430** |

Odd half (held out; the first design was read on it before the final design existed, see dev/m16-notes.md item 3):

| board | connections | plain-8 | control-12 | mixed-copies | mixed-next |
|---|---:|---:|---:|---:|---:|
| sample037 | 58 | 51 | 53 | 51 | 51 |
| sample056 | 57 | 52 | 54 | 52 | 52 |
| sample095 | 65 | 58 | 59 | 58 | 58 |
| sample102 | 54 | 52 | 52 | 52 | 52 |
| sample118 | 67 | 59 | 57 | 59 | 59 |
| sample136 | 62 | 58 | 61 | 58 | 58 |
| sample173 | 60 | 49 | 53 | 49 | 49 |
| sample191 | 69 | 60 | 61 | 60 | 60 |
| **total** | 492 | **439** | **450** | **439** | **439** |

Result: the gate of issue #26 stage 2 ("advised runs route more on a held-out half") is **not met**; see doc 17 section 8.1.

## 2. Checks (`verification/`)

| File | What |
|---|---|
| `unit-advice.txt` | the 8 unit test cases tagged `[advice]` (`tests/test_advice.cpp`) |
| `advisor-scripts.txt` | `advisors/test_advisors.py`: both reference advisors against fake servers |
| `advice-cli.txt` | `tests/integration/advice_cli.py`: the command line end to end on a generated board |
| `quick-compare.txt`, `planes-compare.txt`, `planes-soft-compare.txt`, `mid-compare.txt` | options off: `bench/compare_runs.py --expect-identical`, `origin/main` against this branch |
| `advice-determinism-real-board.txt` | `--advice` on a real board at 1, 2, 4 and 4 threads: one sha256 |
| `drc-advised.txt` | KiCad DRC of advised boards: router-introduced errors |
| `ctest-release.txt`, `ctest-cpu-only.txt` | the full suite, release preset (CUDA) and CPU-only (the CI configuration): 291 of 291 pass; the skips are the five GPU tests on the CPU-only build and `fixture_preparation_native` (it needs KiCad's Python in Docker and passes when run directly: `tests/integration/fixture_preparation_native.py`) |
| `asan-advice.txt` | the 10 new tests, and the 11 router-heavy existing ones that exercise the changed router code, under address + undefined-behaviour sanitizers: all pass (the whole suite was not run under ASan) |

`mid-compare.txt` says `Gates: FAIL` although all 11 boards are byte-identical: the judge introduced by PR #28 reports "evaluated DRC errors are
capped; gate unknown" for `LeeChee_1800`, `front-end-modules_LimeSDR_Sony` and `oskirby_logicbone` (their KiCad reports hit the cap), and
`compare_runs.py` counts an unknown gate as a failure. Comparing the baseline with itself gives the same `FAIL`, so it is not caused by this branch.
