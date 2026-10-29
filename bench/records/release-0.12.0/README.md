# Release 0.12.0 / 0.12.1 records: the test suite and PCBench tiers A-D

2026-10-29, branch `release-0.12.0` on main `4e69750` (pull request #59 merged) with the version bump. Since 0.11.0: test campaign
fixes and generated pours (#50, D137, D138), unfilled zones and hole clearance (#51, D140), the roadmap night (#57, D141-D151), the
per-class cache for custom rules and delay tuning with room (#58, D139, D152), and the search speed-ups (#59, D154). Machine: 56 cores,
**idle** (load average about 2 before the runs, no other jobs, both GPUs free). Judge: `kicad-cli` 10.0.6 (Docker `kicad/kicad:10.0.6`),
the D106 judge, the same as 0.10.0 and 0.11.0. Binary: the `release` preset built from this branch, snapshotted by `run.py` into each
run directory (`runs/*/` holds `summary.json`, `boards.jsonl` and `report.md`; the routed boards stay in the git-ignored `bench/results/`).

## 1. Tests

`ctest --preset release`: 428 of 428 passed, 2 skipped, 1409 s (`ctest.txt`). Both GPU tests ran (the GPUs were free).

- `fixture_preparation_native`: skips without its fixture, as in 0.11.0.
- `delay_room`: skipped in the full run because the worktree had no prepared demos (`build/demos`). Run again with them, it **fails**
  (`delay-room.txt`, issue #60). The route reaches 129/147 and leaves `/Debugger/D+` 8.69 ps out of its 0.5 ps skew. The tuner had brought
  D+ to 78,441.6 ps against D- at 78,504.1 ps, and a stage after tuning shortens it again. 455e315, before D154, passes (125/147, D+ at
  79,427 ps). It also passes with `--reach-after 0` (125/147) and with `--reach-after-cap 0` (127/147). The late proof routes a different
  board, and on that board the post-tuning stage undoes the meanders. Released with this as a known limit, at the owner's decision.

## 2. Tiers A-D, 120 s per board, 8 variants, 12 boards at a time

The same boards as the 0.10.0 and 0.11.0 release runs (`--limit 40` for A and B, 30 for C, the whole tier for D, seed 1).

| Tier | Boards | Run | Clean (D106 judge) | Fully routed, no added error | of which gate *unknown* | Completion | Added errors | Router seconds (sum) |
|---|--:|---|--:|--:|--:|--:|--:|--:|
| A | 40 | `rel0120-tierA` | 40 (100%) | 40 | 0 | 100% | 0 | 1166 |
| B | 40 | `rel0120-tierB` | 29 (72.5%) | 29 | 0 | 97.4% | 0 | 3123 |
| C | 30 | `rel0120-tierC` | 15 (50.0%) | 18 | 3 | 97.5% | 0 | 3043 |
| D | 22 | `rel0120-tierD` | 9 (40.9%) | 12 | 3 | 95.9% | 0 | 2290 |

Against 0.11.0's release runs (`compare-tier*-0.11.0.txt`), clean goes 40 → 40, 28 → 29, 14 → 15, 8 → 9. The new clean boards are
`avr_ledprojector` (B), `kitspace_40-channel-hv-switching-board` (C) and `Keyboard_PCB_Keyboard` (D, 534/534). Open connections fall on
most of the boards that stay open, for example CoreOne 56 → 28, LeeChee 46 → 29, TCKB 31 → 18, poncho 46 → 30 and Aleste 257 → 228. Three
boards have one or two more: kitspace_d20_tri 137 → 139, rvi_v2x 24 → 25 and LFK78 31 → 32. The gate-*unknown* boards are the same as in
0.11.0.

0.11.0 was measured on a shared machine (load 40 to 78); this release on an idle one, and the limit is wall-clock time. For tier D there is a
like-for-like figure: on an idle machine on 2026-10-26, 0.11.0 scored 8 boards and routed the keyboard 530/534
(`bench/records/test-campaign-2026-10/full-test-2026-10-26.md`); 0.12.0 scores 9 here. At a fixed work budget, the speed-ups alone (D154)
were measured in `bench/records/d154-speed` (tier C clean 16 → 17 at 120 s on the same machine, same conditions for both binaries).

## 3. How to reproduce

```
python3 bench/run.py --tier A --limit 40 --time 120 --threads 8 --name rel0120-tierA   # B: --limit 40; C: --limit 30; D: --limit 40
python3 bench/compare_runs.py rel0110-tierA rel0120-tierA
ctest --preset release -R '^delay_room$' --output-on-failure                           # needs bench/prepare_demos.py -> build/demos
```

## 4. Release files gate

The first `v0.12.0` tag failed the install test (`scripts/test_release_install.py`): the example board, `plane_smd`, routed 48/49 with
`--variants 2`. That dates from D143 (#57), not this release: 455e315 also routes 48/49. A84 had moved the ctest for that board to four
variants, but the tag-only install test and the tutorials still used two. All of them now use `--variants 4` (49/49; the tutorials'
sample output is from that run), and the release was made as **0.12.1** from the fixed commit (the `v0.12.0` tag stays, unreleased). 0.12.1 differs from the measured 0.12.0 build only in the version string, this install test, `INSTALL.txt` and the tutorials, so the tables above stand for it.
