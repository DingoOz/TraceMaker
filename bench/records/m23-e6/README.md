# M23 E6: one time limit for the whole run (D186, doc 05 §82)

`run.sh` ran three arms at once on `pcbench-bd-fail` (the 33 tier B-D boards that are not clean at 120 s), 2 jobs each, binary of commit
3589211 (`build/e6-tracemaker.bin`). The machine was shared: the E3 runs (`../m23-e3`) started part-way through and the load reached 68 on 56
cores, so every wall time here is longer than on a quiet machine. All three arms ran under the same load.

| arm | routed | clean | wall, sum s | median s | max s |
|---|---|---|---|---|---|
| `--time 40` (each stage 40 s; today) | 15,871 / 16,935 | 0 | 4,432 | 136 | 168 |
| `--time-total 120` (share 0.5) | 15,909 / 16,935 | 0 | 4,086 | 126 | 144 |
| `--time-total 120 --rescue-share 0.75` | 15,982 / 16,935 | 0 | 4,064 | 124 | 148 |

Per board, routed connections: share 0.75 against share 0.5 better on 11, worse on 3, the same on 19; against `--time 40`, better on 13,
worse on 6, the same on 14. 32 of 33 boards exit 3 (not complete) in every arm.

**Chosen:** `--rescue-share` default 0.75, and the plugin's default `--time-total 120`.

**Overrun.** `--time-total` is a target. 14 of the 33 boards took more than 125 s, the most 148 s (P8000_WDC_Emulator: plain run 90 s
asked, about 96 s taken; its warm relax rescue got the 30 s left and took 45 s, because the relax pass's legalising stage does not stop on the
time limit). The same overrun happens with `--time`, where it comes on top of every stage's own limit.

On a quiet machine the overrun is smaller: `../m23-e5` ran the same 33 boards at `--time-total 120` (two arms at once, 2 jobs each). Median
123 s, at most 134 s, and 5 or 6 boards over 125 s. P8000 alone took 124 s.

Files: `run.sh`; `e6-*/boards.jsonl` and `summary.json` per arm; `e6-{a,b,c}.log`.
