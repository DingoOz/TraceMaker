# M20 records: the run report (doc 18 D3, D136)

Results of the tests and runs behind [docs/18-next-features.md](../../../docs/18-next-features.md) section 9, kept in the repository because
`bench/results/` is git-ignored. How to read a report: [docs/how-to/run-report.md](../../../docs/how-to/run-report.md).

- Base: `main` `efdf1d6`, branch `run-report`, 2026-10-24. Machine: 56 cores, shared with other jobs (load 23 to 35 during the runs); the
  GPUs were full, so cost-to-go fields fell back to the CPU.
- Boards: the plane tier (`bench/sets/planes.txt`, prepared by `bench/prepare_demos.py` into `build/demos/`; not committed, as no fixture is).

## 1. Reports for the plane boards (`<board>/`)

`run_reports.sh`: `tracemaker route <board> -o routed.kicad_pcb --work 3000000 --time 3600 --threads 4 --soft-zones --si-rules --view
--report report.html --json summary.json`, then `tracemaker report summary.json -o report.md` for the Markdown form. Each folder holds the
summary, the report in both forms and the route log. The routed boards are not kept.

| Board | Routed | Unconnected after refill | Time-limited | DRC errors before -> after | Routing errors added | Rules not read | Open by cause | Critical nets |
|---|--:|--:|:-:|--:|--:|--:|---|--:|
| complex_hierarchy | 87/87 | 0 | no | 0 -> 0 | 0 | 0 | - | 0 |
| multichannel_mixer | 173/173 | 3 | no | 0 -> 0 | 0 | 0 | - | 0 |
| pic_programmer | 82/86 | 7 | no | 0 -> 0 | 0 | 0 | boxed_in 4 | 0 |
| plane_smd | 48/49 | 1 | no | 0 -> 0 | 0 | 0 | boxed_in 1 | 2 |
| interf_u | 140/164 | 26 | no | 0 -> 0 | 0 | 0 | not_attempted 22, boxed_in 2 | 2 |
| RoyalBlue54L-Feather | 102/147 | 61 | no | 38 -> 38 | 0 | 0 | not_attempted 37, boxed_in 8 | 6 |
| sonde_xilinx | 48/48 | 0 | no | 0 -> 0 | 0 | 0 | - | 0 |
| StickHub | 121/128 | 10 | no | 0 -> 0 | 0 | 0 | ripped_not_rerouted 6, boxed_in 1 | 18 |

Caveats, honestly:

- **interf_u and RoyalBlue54L-Feather** were re-run on 2026-10-27 with the binary of `main` `ec7cc9b` (after D137's GPU back-off; same
  command). interf_u finished in 128 s: the 20-minute single-core stall of 2026-10-24 was the GPU fallback cliff that D137 fixed.
  RoyalBlue finished in 72 s under `--time 3600`, so `time_limited` is false and its 37 never-attempted connections are left by the
  work budget, not the clock. Its 38 DRC errors (10 `clearance`, 28 `copper_edge_clearance`) are in the input board; the route adds none.
- The unconnected count after the refill equals the report's own DRC count of unconnected items on every board: two independent paths
  (the router's refill count and `tracemaker drc` on the written board) agree.
- An earlier run of these boards with the default 120 s limit routed differently (multichannel_mixer 8 unconnected after refill instead of
  3, StickHub 119 instead of 121 routed): the time limit, not the work, ended those runs. That is why `time_limited` was added (D136).

## 2. Every number traceable (`test-report.txt`)

`tests/test_report.cpp`: the HTML of a summary with every section is parsed; each value carries a `data-src` JSON pointer and its text equals
the pointer's field as the report prints it; with the values, code spans, style and title removed, no digit is left in the page (the
renderer's own words carry no number). The Markdown's footnotes all resolve. Also: the rules section's net counts add up to the board's
nets; the DRC comparison counts a track laid over another net's pad as an added routing error; a summary from a plain `--json` run renders
and says what it lacks. Full unit suite: `tm_tests-full.txt`.

## 3. Options off: no change to the copper (`quick-compare.txt`)

`bench/run.py --set quick --work 1000000 --route-args "--variants 1" --jobs 4` with this branch's binary and with `main`'s, same machine:
routed boards byte-identical 30/30, gates pass (clean pass 12/30, mean completion 89.2 % for both; `--variants 1` is a single router, so
this is below the full portfolio's 25/30). Run before the `time_limited` flag was added; the flag is written when the time check fires
and read by nothing in the router, so it cannot change copper.
