# Release 0.11.0 records: the test suite and PCBench tiers A-D

2026-10-24, branch `release-0.11.0` on main `225b68f` (pull request #49 merged) with the version bump. Since 0.10.0: the documentation
site (#48, D134), the macOS 15 build with a Metal backend (#46, D135) and the run report (`route --report`, #49, D136). Machine: 56
cores, shared with other jobs throughout (load average 40 to 78 from other jobs during the runs, on top of the benchmark's own 12 boards x 8
threads). Judge: `kicad-cli` 10.0.6 (Docker `kicad/kicad:10.0.6`), physical violation instances added against the input plus connectivity
(doc 10 §4, D106), the same judge as 0.10.0. Binary: the `release` preset built from this branch, snapshotted by `run.py` into each run
directory (`runs/*/` holds `summary.json`, `boards.jsonl` and `report.md` of every run; the routed boards stay in the git-ignored
`bench/results/`).

## 1. Tests

`ctest --preset release`: 391 of 391 passed, 3 skipped (the two GPU tests, `GPU Philox fill is bit-identical...` and `cost-to-go field:
concurrent callers...`, which skip when no GPU can be had; both passed when run again right after with both GPUs idle,
`gpu-tests-rerun.txt`; `fixture_preparation_native`, no fixture), 1054 s (`ctest.txt`). Run
before the report's wording fix (singular nouns, a note shared by both DRC sides printed once); after it, `tm_tests "[report]"`
passed again (6 cases, 372 assertions).

## 2. Tiers A-D, 120 s per board, 8 variants, 12 boards at a time

The same boards as the 0.10.0 release runs (`--limit 40` for A and B, 30 for C, the whole tier for D, seed 1).

| Tier | Boards | Run | Clean (D106 judge) | Fully routed, no added error | of which gate *unknown* | Completion | Added errors | Route seconds (sum) |
|---|--:|---|--:|--:|--:|--:|--:|--:|
| A | 40 | `rel0110-tierA` | 40 (100%) | 40 | 0 | 100% | 0 | 1703 |
| B | 40 | `rel0110-tierB` | 28 (70.0%) | 28 | 0 | 97.1% | 0 | 3321 |
| C | 30 | `rel0110-tierC` | 14 (46.7%) | 17 | 3 | 97.4% | 0 | 3191 |
| D | 22 | `rel0110-tierD` | 8 (36.4%) | 11 | 3 | 94.9% | 0 | 2403 |

Against 0.10.0's quoted runs (`compare-tier*-0.10.0.txt`): A, B and C have the same clean count (40, 28, 14) and completion within 0.1
point. Tier D has one board fewer: `Keyboard_PCB_Keyboard` routed 530 of 534 connections (534 in `rel0100-tierD2`, run at load ~20). The
boards whose error gate is *unknown* are the same in both releases (6 in C and 7 in D, of which 3 and 3 are fully routed; capped error
types the boards had before routing, see the 0.10.0 record). No board in
any run has an added routing error.

## 3. Load, not the code: the 0.10.0 binary under the same conditions

The how-to asks for the previous binary beside the new one on a shared machine. The 0.10.0 snapshot binary
(`bench/results/rel0100-tierD2/tracemaker`, `tracemaker version` 0.10.0) was run on tier D with today's `run.py` right after the 0.11.0
runs (`rel0100w-tierD`, load average 33 rising to 78):

| Tier | Binary | Clean (D106) | Fully routed, no added error | Completion | Keyboard_PCB_Keyboard | Route seconds (sum) |
|---|---|--:|--:|--:|--:|--:|
| D | 0.10.0 at release (`rel0100-tierD2`, load ~20) | 9 (40.9%) | 12 | 95.1% | 534/534 | 2385 |
| D | 0.10.0 today (`rel0100w-tierD`, load 33-78) | 7 (31.8%) | 10 | 94.5% | 528/534 | 2527 |
| D | 0.11.0 (`rel0110-tierD`, load 40-66) | 8 (36.4%) | 11 | 94.9% | 530/534 | 2403 |

Under today's load the 0.10.0 binary scores one board below 0.11.0, so the tier D difference is the shared machine at a 120 s wall-clock
limit, not the code. The changes since 0.10.0 do not touch the router's search: the run report reads the result after the route, and the
`time_limited` flag it adds is written when the time check fires and read by nothing in the router (D136). With options off, the quick tier
at a fixed work budget was byte-identical to `main` on 30 of 30 boards (`bench/records/m20-run-report/quick-compare.txt`).

## 4. How to reproduce

```
python3 bench/run.py --tier A --limit 40 --time 120 --threads 8 --name rel0110-tierA   # B: --limit 40; C: --limit 30; D: --limit 40
TM_BINARY=$PWD/bench/results/rel0100-tierD2/tracemaker python3 bench/run.py --tier D --limit 40 --time 120 --threads 8 --name rel0100w-tierD
python3 bench/compare_runs.py rel0100-tierD2 rel0110-tierD
```
