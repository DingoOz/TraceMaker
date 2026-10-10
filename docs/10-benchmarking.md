# 10 — Benchmarking and evaluation

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).
> Dataset facts verified 2026-10-02: [`../research/kicad-and-benchmarks-2026-10.md`](../research/kicad-and-benchmarks-2026-10.md).

## 1. Fixture sets

| Set | Source | Size | Format | Use |
|---|---|---|---|---|
| **FR-fixtures** | Freerouting `fixtures/` (GPL-3.0) | 155 DSN regression boards | DSN | Router robustness; direct comparison with Freerouting |
| **PCBench-FR** | Freerouting `scripts/benchmark` PCBench fixtures (from PCBench, MIT) | ~1,157 boards with unrouted + reference DSN, `.kicad_pcb`, `ground_truth.json` | kicad_pcb + DSN | **Main routing benchmark.** Freerouting publishes nightly results on exactly this set |
| **DAC2020 bm1–bm11** | github.com/DAC-2020-Submission-1703/PCB-Benchmarks | 11 hand-routed manufactured boards | kicad_pcb | Dense/BGA boards; compare vias and layers with the human routing |
| **KiCad demos** | KiCad 10.0 `demos/` | 19 boards | kicad_pcb | Real KiCad projects with schematics: rule ingestion, IPC plugin, schematic-to-board |
| **PCBWorld** | LGAI-Research/PCBWorld (BSD-3 code) | 679 real boards + 2 synthetic generators | kicad_pcb | Comparison with PCBWorld's published baselines (KiCad PNS, OrthoRoute, RL) |
| **RL_PCB** | LukeVassallo/RL_PCB (MIT) | small circuits | kicad_pcb | Placement comparison with a learned placer |
| **Place-and-route set** (own) | KiCad demos + PCBench boards with routing stripped and footprints scattered | built by script | kicad_pcb | Full schematic-to-routed-board evaluation |
| **Synthetic stress** (own) | generated: BGA fanout grids, dense buses, crossing lattices | parameterised | kicad_pcb | Unit benchmarks for escape, global routing and GPU kernels |

Licences differ per set; the harness downloads sets into `bench/data/` (git-ignored) from their sources
and never redistributes them.

`scripts/fetch_fixtures.sh` pins the three default sets to the commits the 0.8.0 results were measured on
(Freerouting `d216f90` of 2026-09-30, DAC 2020 `bee2888`, KiCad 10.0 `7a7d026`), so a fresh clone gets the same
boards, tiers and Freerouting result file; `TM_FIXTURES=latest` or `TM_REF_<SET>=<commit>` overrides a pin. Its
`derived` step builds `bench/data/dac2020_prepared` (`bench/prepare_dac2020.py`) and, once the placer is built, the
from-scratch set S (`bench/make_place_sets.py scratch`). The fresh-clone path was checked command by command against
the upstream repositories, not by a complete download and test run.

### Building unrouted fixtures

`strip_routing` in `bench/prepare_dac2020.py` removes top-level `segment`/`arc`/`via` nodes and keeps everything
else byte for byte (zones and their fills too). `bench/prepare_demos.py` uses it to turn KiCad demo projects and
test boards into `bench/run.py` fixtures (`<out>/<name>/unrouted.kicad_pcb` with the project and custom rules
beside it). The place-and-route sets (unlocked footprints reset to a pile) are built by `bench/make_place_sets.py`.

## 2. Metrics per board

| Metric | Definition |
|---|---|
| **Clean pass** | 100% connections routed **and** zero added KiCad DRC errors (input vs output, `kicad-cli pcb drc`) |
| Completion | routed connections / routable connections (excluding pins proved dead) |
| Added DRC errors | by KiCad violation type |
| Vias, wirelength, bends | totals; also normalised to the reference routing where it exists |
| Wall time, work units | engine-reported, with hardware recorded: the time is the whole route job (every portfolio variant, D86), the work units (`expansions`) are the winning variant's only |
| Placement (P&R set) | HPWL, lower-bound gap, crossings, courtyard overlaps (must be 0), moved parts |
| Determinism | output hash per board at 1 and N threads, GPU on/off |

## 3. Baselines

| Baseline | How it runs |
|---|---|
| **Freerouting** (current release, plus v2.5.0-RC12 as published) | Its own CLI on the DSN; SES imported back into `.kicad_pcb` by TraceMaker's SES reader; judged by the same KiCad DRC. Published nightly figure on PCBench: **74.6% clean, 74.8% fully routed** (v2.5.0-RC12, 1,157 fixtures) |
| **KiCad PNS / PCBWorld baselines** | Published PCBWorld numbers; optionally its environment (pins KiCad 9.0.8) |
| **OrthoRoute** | Published PCBWorld numbers (~1–2% clean pass on mixed boards) |
| **Human reference** | The original routing of PCBench and bm boards (vias, length) |
| **SA-PCB** | For placement HPWL on the P&R set |
| **TraceMaker previous commit** | Regression gate |

## 4. Harness (`bench/`)

`bench/run.py` routes each board of a set with a snapshot of the engine binary, judges input and output with
`kicad-cli pcb drc`, and writes `bench/results/<run-id>/{boards.jsonl, summary.json, report.md, boards/}`
(`summary.json` feeds the progress site). `bench/compare_runs.py` compares two runs board by board and checks the
gates of §5.

| Option | Effect |
|---|---|
| `--set NAME` | the boards of `bench/sets/NAME.txt` (names the set in the summary) |
| `--work N` | deterministic work budget per board, no knowledge base; `--time` becomes a safety stop (3,600 s). Every portfolio variant gets the whole budget, so a board does up to eight times `N` |
| `--threads N` | router threads per board; with `--work` the default is the cores per job (at most 8), since the output does not depend on it (D47), but 1 with `--halving` in the route options, whose budget shares follow the thread count; without `--work` it is also the portfolio size (default 1) |
| `--refill` | KiCad refills zones before judging input and output; needed whenever fills may be stale (`--soft-zones`) |
| `--route-args=...` | extra `tracemaker route` options (also `TM_ROUTE_ARGS`) |
| `--fixtures DIR` | boards from `DIR/<name>/unrouted.kicad_pcb` instead of PCBench; a `.kicad_pro`/`.kicad_dru` beside it is copied next to the output so KiCad judges with the board's rules |
| `--tier`, `--limit`, `--seed` | sample PCBench boards by Freerouting tier instead of a set |

**Sets** (`bench/sets/`):

| Set | Boards | Command | When |
|---|---|---|---|
| `quick` | the 30 PCBench tier-A boards of `--limit 30 --seed 1` | `--set quick --work 1000000 --route-args "--variants 1"` (≈ 30 s with `--jobs 4`) | every routing change |
| `mid` | 11 large, hard PCBench boards | `--set mid --work 20000000` | changes to search, plans or rip-up, which can move results at 20 M while `quick` stays identical |
| `planes` | 7 KiCad demos with zone fills plus `tests/boards/plane_smd`, routing stripped | `bench/prepare_demos.py --set planes`, then `--fixtures build/demos --set planes --work 3000000 --refill`, once plain and once with `--route-args=--soft-zones` | changes to zones, planes and plane targets |

**Comparing.** `bench/compare_runs.py BEFORE AFTER [--expect-identical]` lists every board whose routed output
differs or got worse, the clean-pass and completion totals and router seconds, and exits 1 if a gate fails.
`--expect-identical` adds "every routed board byte-identical" for speed-ups and refactors. Build the "before"
binary from the base commit (a clean `origin/main` worktree) and run both on the same machine.

**Other tools.**

| Tool | Purpose | Status |
|---|---|---|
| `speed_ab.py` | engine speed A/B at a fixed work budget: wall and CPU time, instructions, memory, byte-identical outputs (`--demos` for KiCad demos) | maintained |
| `scripts/drc_parity.py`, `drc_broken_parity.py` | `tracemaker drc` against KiCad on demo and broken boards (ctest `kicad_drc_parity`, `kicad_drc_broken_parity`); counts are capped at KiCad's 199 per type | maintained |
| `quality.py`, `quality_bench.py`, `compare.py`, `human_baseline.py` | routing quality metrics; TraceMaker against Freerouting versions; the designers' own routing judged the same way | maintained |
| `check_env.py`, `prepare_dac2020.py`, `prepare_demos.py`, `make_place_sets.py` | tool checks and fixture preparation | maintained |
| `pair_eval.py`, `feasibility.py`, `crules_detect.py`, `crules_place.py`, `crules_tiers.py` | feature evaluations (diff pairs, escape feasibility, component rules) | kept for re-measuring their features |
| `m6_bench.py`, `place_m8.py`, `place_auto.py`, `place_flip.py`, `place_intent.py`, `place_variants.py`, `compare_placed.py`, `rescore_placed.py`, `dsn_place.py` | milestone evaluations of the global router and placement | historical; results are in the docs that cite them |
| `ses_import.py`, `record_fr.py` | import a Freerouting session; screen-record Freerouting | utilities |
| `cpu_sample.py` | per-thread CPU sampling from `/proc` | Linux only |

**Not built** (planned in the first version of this document): manifest files, a `standard` (~300 boards) and
`full` tier as named sets, `bench/report.py` with an HTML dashboard, and paired statistics (sign test, bootstrap
interval). Seeds: best-of-1 is the headline; best-of-5 is reported separately where a comparison needs it
(PCBWorld reports baselines as best of 5).

**Judge noise.** KiCad's `solder_mask_bridge` count can differ between runs on byte-identical boards when zones
are refilled (StickHub: 17, 15 and 13); read a change in that type alone as noise.

## 5. Gates (used by the roadmap)

1. No board in `quick` gets worse in clean pass or completion (`compare_runs.py`).
2. Zero added KiCad DRC errors on every board that was clean before (`compare_runs.py`).
3. Time within budget on every board (with `--work`, the budget is work units; wall time only stops the job).
4. Routed boards byte-identical unless the change is meant to change results (`compare_runs.py
   --expect-identical`; then the differences are recorded).

## 6. Implementation status (2026-10-02; sets, work budgets and refill 2026-10-08, D81)

- `bench/run.py` samples PCBench boards per Freerouting tier, routes each with a binary snapshot, judges with
  `kicad-cli pcb drc`, and compares with Freerouting's published per-board results (`benchmarks.json`).
- Samples: tier A 40 of 453 boards, tier B 40 of 560, tier C 30 of 122, tier D all 22 (seed 1).
- Added errors count only violations involving a track, via or arc (decision A16); other new KiCad reports are
  kept as diagnostics.
- Results go to `bench/results/<run>/` and the progress site's benchmark panel.
- Latest (`final8`): tier A 100% clean, B 65.0%, C 56.7%, D 50.0% (Freerouting 2.5.0-RC12: 100%, 50.0%, 46.7%,
  36.4%); no router-introduced DRC errors on any board.
- Sets, `--work`, `--refill` and `compare_runs.py` (D81), measured on `origin/main` `a5532a9`:
  - `--set quick --work 1000000 --route-args "--variants 1"` reproduces the earlier quick-tier run (made with
    `TM_ROUTE_ARGS`) byte for byte on all 30 boards: 40.0 % clean, 89.2 % completion, about 30 s with `--jobs 4`.
  - `--set mid --work 20000000`: 0 of 11 clean, 76.4 % completion, 3 minutes with `--jobs 4`.
  - `--set planes --work 3000000 --refill --route-args=--soft-zones`: 86.1 % completion. Without `--refill` the
    judge reads the stale fills, and without the copied project files KiCad judged demo outputs on default rules
    (hundreds of false `track_width` and `clearance` errors on RoyalBlue, StickHub and plane_smd).
- Timing (D86). The summary's `seconds` used to be the winning variant's own time. With `--work` every board runs
  all eight variants, and at one thread per board (the old default) they ran one after another, so a board's job
  took several times the reported figure (more than eight times when the winner is one of the quicker variants):
  on the `mid` set at 20 M, 6–16 s reported against 45–107 s of process time. `seconds` now covers the whole job,
  `variant_seconds` keeps the winner's time and `variants` lists every variant. A per-variant profile
  (decelerator4030, LimeSDR, logicbone at 5 M and 20 M) put 81–89 % of the process in the seven losing variants
  and under 2 % in reading, set-up, clean-up and writing. Times in runs made before D86, including the Freerouting
  comparisons, are the winner's only.
