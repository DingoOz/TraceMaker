# D154: search speed

Doc 05 §62, decision D154. Linux, GCC 15, the `cpu-only` preset (RelWithDebInfo), CPU fields, KiCad 10.0.6 as judge. The machine
was otherwise idle unless noted.

## Binaries

| Name | Commit | What it is |
|---|---|---|
| base | 455e315 (main) | before |
| fi | 5b9bfbf + fd89f29 | vectorised CPU field and banded inside-board test (same output) |
| lazy | f2a7238 | + chunked per-class caches (same output) |
| exp1–exp5 | working tree | the search changes while they were developed (options; see `ab/`) |
| new | this branch | late reachability proof on by default (100,000 / cap 10,000) |

The `commit` field in each `summary.json` is the commit of the checkout `bench/run.py` ran from. It is not the commit of the binary,
which was set with `TM_BINARY`. Tier C used stripped copies of the same two binaries (the disk was full).

## Files

- `profile/<board>-flat.txt`: gprof flat profiles, 455e315, one variant, 20 M work.
- `profile/<board>-searches.txt.gz`: one line per search from a throwaway build of 455e315: found, expansions, soft, open list
  exhausted, touched the window edge, capped, distinct cells, window cells. `scripts/simulate_late_proof.py` reads them.
- `ab/<run>/<board>/`: `route.json` and stderr (unrouted lines removed) of the five-board A/B runs (`scripts/abrun.sh`, compared with
  `scripts/abcmp.py`): one variant, one thread, `--work 20000000 --no-gpu --seed 7`. Runs: `base`, `fi`, `lazy`;
  `ra100k`/`ra30k` (trigger, no cap), `c10`/`c30`/`c10t50` and `R`/`R50` (with caps), `ver` (`--reach-verify`, trigger 20,000),
  `F`/`fa2k` (`--field-after 2000`), `tie`/`RT`/`RFT` (`--tie-deep`), `R10`/`R5` (the default at 10 M and 5 M work), `b40`/`b80` (base at
  40 M and 80 M), `t_ref1`/`t_5` (timing of the default path against `lazy`).
- `runs/<set>-{base,new}/`: `summary.json`, `boards.jsonl`, `report.md` of each tier run; `runs/<set>-compare.txt` from
  `bench/compare_runs.py`.
- `scripts/fieldbench.cpp`: the field micro-benchmark (450 × 450 × 4, 22 rounds: reference 726 ms, row sweeps 193 ms with
  vectorisation).

## Results

Five boards, one variant, 20 M work (routed / CPU s):

| Board | base | default (100 k, cap 10 k) | default at 10 M | base at 40 M | base at 80 M |
|---|---|---|---|---|---|
| kitspace_d20_r0.1 | 228 / 14.2 | 259 / 22.1 | 250 / 9.2 | 256 / 30.0 | 259 / 56.9 |
| oskirby_logicbone | 804 / 21.3 | 883 / 30.0 | 858 / 17.7 | 883 / 41.8 | 909 / 72.0 |
| sbc_sbc | 310 / 14.4 | 341 / 20.8 | 327 / 9.5 | 329 / 29.1 | 347 / 60.3 |
| KiCad-Library_Teensy_test_layout | 1,090 / 16.4 | 1,093 / 17.6 | | | |
| LeeChee_1800 | 1,010 / 18.3 | 1,010 / 18.9 | | | |

(base here is `lazy`: same output as 455e315, less time.)

Tiers, base → new:

| Set | Budget | Result | Output | Route CPU |
|---|---|---|---|---|
| quick (30) | 1 M work, 8 variants | clean 26 → 26 | 30/30 byte-identical | 385 → 299 s |
| mid (11) | 20 M work, 8 variants | 4 boards changed: decelerator +10, LimeSDR +3, EEZ +21, logicbone ±0 | 7/11 identical | 6,157 → 4,357 s |
| mid (11) | 60 s, 8 variants | routed 9,349 → 9,401 (EEZ +33, LeeChee +22, MonApollo +11, kitspace_d20 +7, logicbone +5, LimeSDR +3; Teensy −10, decelerator −10, Aleste −8, sbc −1) | | 5,355 → 5,347 s |
| B (40) | 120 s, 8 variants | clean 29 → 29, routed 5,197 → 5,198 | 12/40 identical | 16,935 → 16,036 s |
| C (30) | 120 s, 8 variants | clean 16 → 17 (mechkeys_58r-mxalps), routed 9,529 → 9,537 (+3 on three boards, +1 on one, P8000 −2) | 7/30 identical | 19,254 → 18,476 s |

`mid` prints `Gates: FAIL` for both runs alike: KiCad's DRC list is capped on LeeChee, LimeSDR and logicbone (see
`bench/records/m16/README.md`).

Tests: `cpu-only` ctest 427/427 and the GPU field tests in `release` pass. Under the `asan` preset, the `[field]`, `[route]`, `[geom]`,
`[rules]`, `[txn]`, `[stall]` and `[causes]` unit tests (135 cases, 988,239 assertions) and the `reach_verify`, `late_reach_verify` and
`thread_determinism` integration tests pass with no sanitizer reports.
