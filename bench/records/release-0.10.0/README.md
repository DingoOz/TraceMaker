# Release 0.10.0 records: the test suite and PCBench tiers A-D

2026-10-18, branch `release-0.10.0` on main `374719b` (pull request #47 merged) with the version bump. Machine: 56 cores, no GPU used by the
router (the P100 and V100 were free, but `bench/run.py` routes on the CPU). Judge: `kicad-cli` 10.0.6 (Docker `kicad/kicad:10.0.6`),
physical violation instances added against the input plus connectivity (doc 10 §4, D106). Binary: the `release` preset built from this
branch, snapshotted by `run.py` into each run directory (`runs/*/` holds `summary.json`, `boards.jsonl` and `report.md` of every run;
the routed boards stay in the git-ignored `bench/results/`).

**The machine was shared with other jobs throughout** (load average 100 to 125 during the first runs, 15 to 46 during the second ones,
on top of the benchmark's own 12 boards x 8 threads). At a 120 s wall-clock limit that matters: see §3.

## 1. Tests

`ctest --preset release`: 379 of 379 passed, 1 skipped (`fixture_preparation_native`, no fixture), 1278 s under that load (`ctest-release.log`).

## 2. Tiers A-D, 120 s per board, 8 variants, 12 boards at a time

The same boards as the 0.9.0 release runs (`--limit 40` for A and B, 30 for C, the whole tier for D, seed 1; checked board for board).

| Tier | Boards | Run | Clean (D106 judge) | Fully routed, no added error (the 0.9.0 criterion) | of which gate *unknown* | Completion | Added errors | Route seconds (sum) |
|---|--:|---|--:|--:|--:|--:|--:|--:|
| A | 40 | `rel0100-tierA` | 40 (100%) | 40 | 0 | 100% | 0 | 1595 |
| B | 40 | `rel0100-tierB` | 28 (70.0%) | 28 | 0 | 97.2% | 0 | 3390 |
| C | 30 | `rel0100-tierC` (load ~100) | 13 (43.3%) | 16 | 3 | 97.3% | 0 | 3187 |
| C | 30 | `rel0100-tierC2` (load ~20) | **14 (46.7%)** | 17 | 3 | 97.4% | 0 | 3163 |
| D | 22 | `rel0100-tierD` (load ~100) | 8 (36.4%) | 11 | 3 | 95.2% | 0 | 2390 |
| D | 22 | `rel0100-tierD2` (load ~20) | **9 (40.9%)** | 12 | 3 | 95.1% | 0 | 2385 |

The release notes and the README quote the second C and D runs. For comparison, the 0.9.0 release runs of 2026-10-08 (`rel090-tier*`,
idle machine, the judge of that time): 40, 29, 18 and 12 clean (100 / 72.5 / 60.0 / 54.5 %), completion 100 / 97.4 / 97.6 / 96.7 %.

### The *unknown* gate

Since D106 a board is *unknown*, never clean, when an error type at KiCad's report cap (199, or 499 for clearance) occurs on either
side, because the omitted instances could involve routed copper. All six such boards here are fully routed with no added error, and the
capped type is one the board had before routing:

| Tier | Board | Capped type at the limit |
|---|---|---|
| C | `Microdox-PCB_Microdox` | `solder_mask_bridge` (199 before and after) |
| C | `Microdox-PCB_Microdox-topMCU` | `solder_mask_bridge` |
| C | `wordclock_ledboard2` | `annular_width` |
| D | `Hardware_Playground_heater_actuator_node` | `annular_width` |
| D | `badge2016_Badge_init` | `solder_mask_bridge` |
| D | `perfplusplus_Ard-perf++` | `clearance`, `hole_clearance`, `shorting_items`, `solder_mask_bridge` |

The 0.9.0 judge counted all six clean. No board in any run has an added routing error (`error_gate` is `pass` or `unknown`, never `fail`).

### Boards that differ from the 0.9.0 runs by the 0.9.0 criterion

| Tier | Board | 0.9.0 (2026-10-08) | 0.10.0 (second run) |
|---|---|---|---|
| B | `avr_ledprojector_avr_ledprojection-0402` | clean | 1 connection open |
| C | `kitspace_40-channel-hv-switching-board` | clean | 2 open |
| C | `mechkeys_MF68+10` | clean | 1 open |
| C | `Hardware_Playground_multisensor_cr2032` | 1 open | clean |

## 3. Load, not the code: the 0.9.0 binary under the same conditions

`run.py` saves the engine binary in each run directory; `rel090-tierC/tracemaker` is the 0.9.0 release binary of 2026-10-08. It was run
today behind a two-line wrapper that drops `--state-out` (an option `run.py` added after 0.9.0), with everything else the same
(`rel090w-tier*`). The old binary reports no `error_gate` fields of its own; the judge is today's in all rows.

| Tier | Binary | Clean (D106) | Fully routed, no added error | Completion | Route seconds (sum) |
|---|---|--:|--:|--:|--:|
| B | 0.9.0 today (`rel090w-tierB`, load 16-27) | 29 (72.5%) | 29 | 97.3% | 2605 |
| B | 0.10.0 (`rel0100-tierB`, load ~100) | 28 (70.0%) | 28 | 97.2% | 3390 |
| C | 0.9.0 today (`rel090w-tierC`, load 24-40) | 14 (46.7%) | 17 | 97.4% | 2825 |
| C | 0.10.0 (`rel0100-tierC2`, load ~20) | 14 (46.7%) | 17 | 97.4% | 3163 |
| D | 0.9.0 today (`rel090w-tierD`, load 24-41) | 8 (36.4%) | 11 | 95.1% | 2172 |
| D | 0.10.0 (`rel0100-tierD2`, load ~20) | 9 (40.9%) | 12 | 95.1% | 2385 |

Search expansions reached in the 120 s, tier C, median over the 30 boards: the 0.9.0 binary today reached 0.53 of what the same binary
reached in the 0.9.0 release run; the 0.10.0 binary reached 1.00 of the 0.9.0 binary today. Boards that finish early (under 10 M
expansions) show identical counts in all three runs. So the gap to the 0.9.0 table is the shared machine and the judge, not the router.
Also: the quick tier at a fixed work budget (`--work`) was byte-identical on all 30 boards before and after the real-world study's changes
(pull request #47), and the release files of D121 are built from this same source.

## 4. How to reproduce

```
python3 bench/run.py --tier A --limit 40 --time 120 --threads 8 --name rel0100-tierA   # B: --limit 40; C: --limit 30; D: --limit 40
TM_BINARY=/path/to/wrapper python3 bench/run.py --tier C --limit 30 --time 120 --threads 8 --name rel090w-tierC
```

The wrapper (`old-binary-wrapper.sh`) strips `--state-out`/`--state-in` and executes the 0.9.0 binary.
