# M23 E5: throughput (D189, doc 05 §83)

## 1. Does more search route more? (`run.sh`, no argument)

The 33 `pcbench-bd-fail` boards, `--no-rescue`, deterministic work, both arms at once (`e5-work5m`, `e5-work40m`):

| work | routed / 16,935 | open | boards better / worse / same at 40 M | wall, sum s |
|---|---|---|---|---|
| 5 M | 15,409 | 1,526 | | 675 |
| 40 M | 16,034 | 901 (−41 %) | 26 / 0 / 7 | 2,401 |

Yes: on 26 of 33 boards. DronPi is one of the exceptions. On a quiet machine its best variant did 87 M expansions in 90 s and routed the
same 341 of 352 as at 5.9 M (the loaded E6 run). About +210 connections per doubling of the work, on this set.

## 2. Where the time goes (`profile/`)

gprof, DronPi, variant 7, one thread, CPU fields, 6 M work, built with `-pg -fno-ipa-icf -fno-ipa-sra -fno-ipa-cp -fno-partial-inlining`
(without the last three, gprof charges clones to the symbol before them: the open list's heap showed as a `std::function` manager of
`plan_escape_reservations`). Fields 22 %, the search loop 13 %, the open list's heap 9 %, routed-copper checks 7 %, fixed-copper codes 6 %.

The heap's 9 % is mostly gprof's per-call cost. A 4-ary heap with the same pop order (tried, removed) gained 0.4-0.5 % in wall time on
DronPi and Aleste.

## 3. The CPU field skips rows that cannot change (`field-timing/`)

One variant, one thread, `--no-gpu`, 10 M work, two runs each (`one-variant-times.txt`). The output boards are identical (`md5.txt`).

| board | wall before / after, s | CPU fields before / after, s | ns per field cell before / after |
|---|---|---|---|
| DronPi | 16.20 / 14.81 (−8.6 %) | 2.98 / 1.56 | 79 / 41 |
| Aleste | 18.71 / 18.11 (−3.2 %) | 1.54 / 0.95 | 63 / 39 |
| mini_mass | 39.39 / 28.64 (−27 %) | 19.93 / 9.29 | 53 / 25 |

Eight variants, 8 threads, 20 M work, GPU available (`portfolio-times.txt`): mini_mass 61.07 → 58.83 s, Aleste 36.34 → 33.13 s, outputs
identical. With every field on the CPU, mini_mass took 79.94 s, so the GPU threshold (D158, 500 k cells) stays.

The quick tier (30 boards, 1 M work, one variant; `e5f-quick-base`, `e5f-quick-new`): all 30 output boards are byte-identical.

## 4. Equal wall time (`run.sh time`, `run.sh time-cpu`)

The 33 boards, `--time-total 120`, old binary against new, both at once on a quiet machine:

| | routed old / new | better / worse / same |
|---|---|---|
| GPU (`e5t-*`) | 16,120 / 16,133 (+13) | 7 / 3 / 23 |
| `--no-gpu` (`e5c-*`) | 16,132 / 16,114 (−18) | 4 / 8 / 21 |

Both differences are within the noise of time-limited runs. Section 1 predicts about +15 to +20 from a speed-up of this size (a tenth of a
doubling), which this sample cannot resolve. The gate (more routed at equal wall time) is not met.
