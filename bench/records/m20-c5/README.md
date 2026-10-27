# M20 C5 records: open connections by cause, and strict searches from the crowded end

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 55, decision D143, doc 18 section 13. Branch `m20-c5-fanout` from
`main` `ec7cc9b`, 2026-10-27. Machine: 56 cores shared with other jobs; every run is budgeted by work (`--work`), so its copper does not
depend on the load, and KiCad 10.0.6 judges (`--refill` where shown).

## Files

| File | What |
|---|---|
| `causes.md`, `causes.json`, `causes/` | `bench/failure_causes.py --run c3-bd-base --name c5-causes --extra-layers 2`: every open connection of the 33 failing tier B-D boards (`bench/sets/pcbench-bd-fail.txt`, run `c3-bd-base`, 10 M work) classified by `tracemaker causes`, and each board with two more inner layers |
| `work-profile.md` | where the work of run `c5-base` went: first pass against negotiation, and which open connections were ever tried in negotiation |
| `runs/*/` | `boards.jsonl`, `summary.json` and `report.md` of each run below |
| `variants.json`, `portfolio_subsets.py`, `portfolio-subsets.md` | each board's routed count per portfolio variant in six runs, and the score of every subset of variants that could search from the crowded end |

## Runs

All with `bench/run.py --work W --jobs 3`; the binary is the one `run.py` snapshotted into the run (`c5x-*`: `TM_BINARY` pointed at
the final build of this branch).

| Run | Set | Work | Options | What |
|---|---|--:|---|---|
| `c5-base` | `pcbench-bd-fail` (33) | 10 M | `--refill --threads 8`, `--no-gpu` | base |
| `c5-halo03` | same | 10 M | an experimental pin halo (removed) | 2 pitches per step within 0.3 mm of another part's pad |
| `c5-cap100k` | same | 10 M | an experimental strict-search cap (removed) | strict searches stop at 100,000 expansions |
| `c5-share07` | same | 10 M | an experimental first-pass share (removed) | the strict pass stops at 70 % of the budget |
| `c5-crowded` | same | 10 M | `--crowded-source` with routed copper only (an earlier build) | every variant |
| `c5-crowdpads` | same | 10 M | `--crowded-source`, routed copper and the part's own pads (`TM_CROWD_PADS=1` in that build; now the only behaviour) | every variant |
| `c5c-base`, `c5c-crowded` | `pcbench-bd-clean` (59) | 10 M | as `c5-base`, and with `--crowded-source` (pads) | the boards the 0.9.0 run finished clean |
| `c5m-base`, `c5m-crowded` | `mid` (11) | 20 M | `--threads 4 --no-gpu`; crowded = routed copper only | |
| `c5q-base`, `c5q-crowded` | `quick` (30) | 1 M | `--jobs 4 --threads 4`; crowded = routed copper only | |
| `c5x-fail`, `c5x-clean`, `c5x-mid`, `c5x-quick` | the four sets | as above | none: the default portfolio, crowded source in variants 1, 5, 6, 7 | the change as merged |

## Results

| 33 failing boards, 10 M work | Open | KiCad unconnected | Better / worse boards | Clean |
|---|--:|--:|---|--:|
| `c5-base` | 1,383 | 1,368 | | 0 |
| `c5-halo03` | 1,394 | 1,378 | 6 / 9 | 0 |
| `c5-cap100k` | 1,422 | 1,407 | 6 / 15 | 0 |
| `c5-share07` | 1,536 | 1,521 | 1 / 16 | 0 |
| `c5-crowded` (all variants) | 1,217 | 1,203 | 18 / 9 | 1 |
| `c5-crowdpads` (all variants) | 1,204 | 1,188 | 19 / 8 | 1 |
| **`c5x-fail` (default: variants 1, 5, 6, 7)** | **1,220** | **1,204** | 18 / 6 | 1 |

| Set | Base | Crowded source in every variant | Default (variants 1, 5, 6, 7) |
|---|---|---|---|
| 59 previously clean boards, 10 M | 69 open, 44 clean | 49 open, 44 clean (AmpOne and bumps lose clean, two others complete) | 46 open, 46 clean, 1 board worse by one connection |
| mid, 11 boards, 20 M | 1,306 open | 1,074 (routed copper only) | 1,093; 7 better, 2 worse by one connection each |
| quick, 30 boards, 1 M | 32 open, 25 clean | 20 open, 27 clean (routed copper only) | 24 open, 26 clean, no board worse |

No board in any run has an added KiCad error. The default's subset was chosen with `portfolio_subsets.py c5-base:c5-crowdpads
c5c-base:c5c-crowded` at a time when 38 of the 59 clean boards had finished. The other 21, the mid tier and the quick tier were not
used, and the `c5x-*` runs check the real binary rather than the simulation (they agree: 69 → 46 on the clean set, simulated and
run).

`--polish` was tried on a few boards early and was worse on three; it was not run on all. Classification and runs are one seed.
