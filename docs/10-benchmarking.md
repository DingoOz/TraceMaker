---
status: current
code: [bench]
---

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
| **tscircuit SRJ19 / SRJ20** | github.com/tscircuit `dataset-srj19` (MIT), `dataset-srj20` (no licence file) | 2 × 200 synthetic BGA breakout problems, two layers | Simple Route JSON, converted to kicad_pcb | Dense-package escape with a public reference figure (§7) |
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

`strip_routing` in `bench/prepare_dac2020.py` removes every top-level `segment`/`arc`/`via` node and keeps
everything else byte for byte, including zones. Both DAC2020 and `bench/prepare_demos.py` then prepare the
unrouted derivative through `bench/prepare_inputs.py`: private native `pcbnew.LoadBoard`/`SaveBoard`
conversion **without refill**, deterministic identity repair and item ordering, then native refill/DRC.
Unique nonzero native source UUIDs are preserved; loader-generated, missing, zero and duplicate identities
are repaired before the first authoritative refill. Stale fill caches are discarded, not zone geometry.
This does **not** change the all-tracks/arcs/vias strip policy or add thermal-array retention.

Fixtures contain `<out>/<name>/unrouted.kicad_pcb`, byte-exact copies of the source `.kicad_pro`/`.kicad_dru`
when present, and `preparation.drc.json`. DAC2020 also keeps its byte-exact human `raw.kicad_pcb` and Specctra
fixture. Preparation never rewrites source/human boards. Work is private and the runnable board is published
last; failure removes only generated unrouted board/rule/report artifacts, so an earlier successful board
cannot masquerade as the requested input. Missing current rules do not reuse a previous fixture's rules.
DRC findings are retained as evidence, not treated as a requirement that the unrouted input be clean.

**Tools and reproducibility (D107).** Host Python 3.8+ and its standard library suffice; conversion additionally
needs KiCad 10's `pcbnew` Python bindings, with the exact major.minor.patch version of the selected CLI.
`KICAD_CLI` and `KICAD_PYTHON` select executable paths (not shell command strings). Without overrides,
preparation finds `kicad-cli` on PATH, falling back to `scripts/kicad-cli`; the repository ships an executable
`scripts/kicad-python` companion using the same `TRACEMAKER_KICAD_IMAGE` (default `kicad/kicad:10.0.6`),
home/current-directory mounts and caller UID/GID as the CLI wrapper. Native CLI installs
use the host Python, or KiCad.app's bundled Python on macOS; set `KICAD_PYTHON` when `pcbnew` belongs to a
different interpreter. A version mismatch fails before loading/refilling the board. For Docker, source,
checkout and output must be under `$HOME` or the mounted current directory (the wrappers' mount contract);
no personal paths or additional packages are required.

With the same source/rule bytes, fixture name and native tool versions, repeated preparations should yield
byte-identical **boards and rules**. Native DRC report ordering/counts are not promised byte-identical
(§4, judge noise); report hashes are not a determinism claim. Native conversion/refill intentionally changes
the prepared input compared with the old strip-only scripts: rebuild inputs and measure fresh paired
baselines against the same new fixtures, never compare old cached inputs with newly prepared ones.

Fixture-independent controls (own generated legacy board, no downloaded/redistributed PCB) are:

```
python3 tests/integration/fixture_preparation.py
python3 tests/integration/fixture_preparation_native.py
```

The native control first converts legacy zero-timestamp zones without refill and reports the missing/zero
zone IDs, then invokes the actual demo entrypoint twice and compares board/project/custom-rule bytes,
nonzero unique identities, preserved zone geometry, native fill presence and untouched source bytes. KiCad
unavailability skips the native test; conversion/preparation failures do not. The place-and-route sets
(unlocked footprints reset to a pile) remain built by `bench/make_place_sets.py`.

## 2. Metrics per board

| Metric | Definition |
|---|---|
| **Clean pass** | 100% connections routed **and** zero added KiCad DRC errors (input vs output, `kicad-cli pcb drc`) |
| Completion | routed connections / routable connections (excluding pins proved dead) |
| Added DRC errors | physical violation multiset difference, grouped by KiCad type for display (D106) |
| Vias, wirelength, bends | totals; also normalised to the reference routing where it exists |
| Wall time, work units | engine-reported, with hardware recorded: the time is the whole route job (every portfolio variant, D86), the work units (`expansions`) are the winning variant's only |
| Work profile | per phase (setup, first pass, negotiation, clean-up): expansions, flood cells, searches, cell checks, fixed-copper checks, cost-field cells and fields, commits, rips; deterministic, in the route JSON as `work` (winner) and `work_all_variants` (D88) |
| Route CPU time | user + system seconds of the `tracemaker route` process (`cpu_s` per board, `cpu_seconds` per run) |
| Placement (P&R set) | HPWL, lower-bound gap, crossings, courtyard overlaps (must be 0), moved parts |
| Style (informational) | per board `style`: connections, bends per connection, acute angles, stair-steps, short and off-angle segments, detour ratio, long detours (definitions in doc 05 §39.2), measured on the new copper by the engine and on the fixture's own routing (`style_ref`) by `tracemaker style`; totals in `summary.json` (`style`, `style_ref`, `style_same_boards`) and `report.md`; `compare_runs.py` lists boards whose metric changed. Never a gate |
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
binary from the base commit (a clean `origin/main` worktree) and run both on the same machine. Route CPU time is
printed per board and in total. When both runs used the same `--work`, boards whose output is byte-identical did the
same work, so their CPU change is run-to-run noise or a change in speed that leaves the output alone; the report
prints that band and lists the boards (and the total) that moved beyond it, and at least 10 %. When every identical
board moved the same way by more than 10 % it says that too: one pair of runs cannot tell a faster binary from a
quieter machine, a second run of one binary can. All of it is a pointer for gate 5, not a failure.

**Other tools.**

| Tool | Purpose | Status |
|---|---|---|
| `speed_ab.py` | engine speed A/B at a fixed work budget: wall and CPU time, instructions, memory, byte-identical outputs (`--demos` for KiCad demos) | maintained |
| `scripts/drc_parity.py`, `drc_broken_parity.py` | `tracemaker drc` against KiCad on demo and broken boards (ctest `kicad_drc_parity`, `kicad_drc_broken_parity`); counts are capped at KiCad's 199 per type | maintained |
| `refill_parity.py` | `tracemaker drc --refill-zones` against `kicad-cli pcb drc --refill-zones`: unconnected items per net on boards with zones (`--dir … --glob raw.kicad_pcb --sample N` for PCBench; doc 05 §36) | maintained |
| `quality.py`, `quality_bench.py`, `compare.py`, `human_baseline.py` | routing quality metrics; TraceMaker against Freerouting versions; the designers' own routing judged the same way | maintained |
| `check_env.py`, `prepare_dac2020.py`, `prepare_demos.py`, `make_place_sets.py` | tool checks and fixture preparation | maintained |
| `pair_eval.py`, `feasibility.py`, `crules_detect.py`, `crules_place.py`, `crules_tiers.py` | feature evaluations (diff pairs, escape feasibility, component rules) | kept for re-measuring their features |
| `make_bus_boards.py`, `bus_eval.py` | generated bus boards and the bus evaluation (doc 05 §38): routes without and with the bus options at one budget, measures with `tracemaker buses`, judges with KiCad | kept for re-measuring bus routing |
| `m6_bench.py`, `place_m8.py`, `place_auto.py`, `place_flip.py`, `place_intent.py`, `place_variants.py`, `compare_placed.py`, `rescore_placed.py`, `dsn_place.py` | milestone evaluations of the global router and placement | historical; results are in the docs that cite them |
| `ses_import.py`, `record_fr.py` | import a Freerouting session; screen-record Freerouting | utilities |
| `cpu_sample.py` | per-thread CPU sampling from `/proc` | Linux only |

**Not built** (planned in the first version of this document): manifest files, a `standard` (~300 boards) and
`full` tier as named sets, `bench/report.py` with an HTML dashboard, and paired statistics (sign test, bootstrap
interval). Seeds: best-of-1 is the headline; best-of-5 is reported separately where a comparison needs it
(PCBWorld reports baselines as best of 5).

**Judge noise.** KiCad's `solder_mask_bridge` count can differ between runs on byte-identical boards when zones
are refilled (StickHub: 17, 15 and 13); read a change in that type alone as noise.

**Native error gates (D106).** `bench/drc_judge.py` compares physical error instances, not totals by type:
removing one clearance cannot cancel a new clearance somewhere else. Item ordering and regenerated UUIDs or
footprint references do not change an identity when positions are available; kind, net, layer, geometry
description and position do. Coordinates and dimensions are normalised to nanometres from report units;
items without positions use UUIDs as a fallback. Duplicate findings retain their multiplicity.
Violation-level layer context is also identity: multilayer zone/PTH pad labels may not identify the affected
layer of a thermal defect. Changing its layer is a new instance; changing its minimum/actual spoke-count
diagnostic on the same layer is not.

Routing gates include error-severity `starved_thermal` and `isolated_copper` even if KiCad names only pads or
zones: routed copper can damage a refill without appearing in its report. Other pad/graphic-only changes
remain diagnostic, including pad-only solder-mask refill noise and net-propagation clearance changes.
Placement compares its existing placement-error types by instance, excluding internal errors of one rigid
footprint; thermal/island findings are evaluated because they depend on surrounding zones, not only that
footprint. Routing's existing immutable/type exclusions otherwise stay unchanged.

KiCad 10.0.x reports may truncate at 499 clearance findings and 199 findings of other types, across severities
(constants and assumption isolated in the helper). Reaching a cap on either side makes that type's difference
unknown; it is not reported as an exact added count. A capped evaluated type with errors on either side sets
`error_gate` (or `place_error_gate`) to `unknown`, never clean or a passing comparison, even if the visible
errors name only pads. Warning-only caps and routing-immutable types remain diagnostic, with evaluated and
non-evaluated capped types recorded separately. `added_errors`, quality's `router_errors`, and the existing
CLI options retain their meanings and type-count display, now backed by instance differences.

**Connectivity limitation.** Completion deliberately retains the existing ratio from KiCad's
`unconnected_items` list. That list may truncate at 499; `connectivity_capped` discloses this independently
of evaluated error caps. The ratio is not an exact connectivity measurement at saturation. No uncapped
connectivity worker or new completion definition is included here.


## 5. Gates (used by the roadmap)

1. No board in `quick` gets worse in clean pass or completion (`compare_runs.py`).
2. Zero added KiCad DRC errors on every board that was clean before (`compare_runs.py`).
3. Time within budget on every board (with `--work`, the budget is work units; wall time only stops the job).
4. Routed boards byte-identical unless the change is meant to change results (`compare_runs.py
   --expect-identical`; then the differences are recorded).
5. Quality is judged at equal work, speed at equal wall time. The work budget charges search expansions and flood
   cells only (the work profile shows what else a run did), so a change that moves work into dearer operations, such
   as reaching negotiation sooner, takes longer at the same work while routing more in the same time (doc 05 §37).
   A claim that a change is faster or slower needs runs at equal wall time; `compare_runs.py` points at the boards.

## 6. Implementation status (2026-10-02; sets, work budgets and refill 2026-10-08, D81)

- `bench/run.py` samples PCBench boards per Freerouting tier, routes each with a binary snapshot, judges with
  `kicad-cli pcb drc`, and compares with Freerouting's published per-board results (`benchmarks.json`).
- Samples: tier A 40 of 453 boards, tier B 40 of 560, tier C 30 of 122, tier D all 22 (seed 1).
- Added errors compare physical instances involving a track, via or arc (decision A16), plus error-severity
  thermal/island findings after refill (D106); other new pad-only reports remain diagnostics. Evaluated capped
  error reports are unknown, not clean.
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
- Work profile and route CPU time (D88). The counters change no output (`quick` 30/30 byte-identical) and cost no
  measurable time. Two `mid` runs at 20 M (`--jobs 4 --threads 3`, the same work), `origin/main` against D87 (#21):
  the four byte-identical boards moved +9 % to +14 % in CPU time between the runs (noise of one sequential pair on
  this 14-core Mac, or a slow-down that left their output alone: one pair cannot tell);
  only sbc (+41 %) and kitspace_d20 (+24 %) moved beyond it. A 10 % threshold
  alone would have flagged seven boards. Single CPU-time comparisons of under about 15 % are not evidence here.

## 7. tscircuit BGA breakouts: sets `srj19` and `srj20` (D90, measured 2026-10-11)

The dense-package work (M9) had no public reference figure. tscircuit publishes 400 synthetic BGA breakout
problems in Simple Route JSON and a dashboard with its own router's results on them, so these two sets give one.
The format support is doc 08 §9.

**Data.** Fetched into `bench/data/tscircuit/` at pinned commits by `scripts/fetch_fixtures.sh tscircuit`, never
copied into this repository:

| Set | Source | Commit | Licence | Problems |
|---|---|---|---|---|
| `srj19` | github.com/tscircuit/dataset-srj19 (BGA breakouts with passive components on the other side) | `c1206f3` | MIT | 200, two layers, 11–87 connections (7,395), 0.15 mm traces, vias 0.3 / 0.2 mm |
| `srj20` | github.com/tscircuit/dataset-srj20 (BGAs with missing balls) | `9384fb8` | none stated (no licence file, none in `package.json`): local benchmarking only | 200, two layers, 11–108 connections (8,715), 0.12 mm traces, no via size given |

Both use a 0.8 mm ball pitch with 0.36 mm square pads, every connection has two points, and neither states a
clearance, a board-edge rule or blind vias.

**Conversion.** `bench/prepare_srj.py` (run by the fetch step once the engine is built) converts every problem
with `tracemaker srj-import --clearance-mm 0.1 --hole-to-hole-mm 0.1` into
`bench/data/tscircuit/boards/<set>/<sample>/unrouted.kicad_pcb` with its project. The two distances are those of
tscircuit's published pass criterion (below), so KiCad's judge and theirs test the same thing; the importer's own
default (0.15 mm, tscircuit's router fallback, and no hole rule) is reported as a variant. Via size for `srj20` is
tscircuit's fallback (0.3 / 0.15 mm). The script checks every board by counting: connections in the problem =
ratsnest the board implies = connections `tracemaker route` reports. All 400 convert and all 400 pass.
`bench/sets/srj19.txt` and `srj20.txt` name the fixture directory in a `#fixtures:` line, which `bench/run.py`
now reads, so the sets run by name.

**Judges.** The primary judge is KiCad's DRC with the board's rules; a board is a clean pass when
nothing is unconnected, no evaluated error instance was added, and no evaluated error type is capped (D106).
The second score is tscircuit's own.
`bench/srj_score.py` exports every routed board with `tracemaker srj-export` and runs tscircuit's
`evaluateRelaxedDrc` on it: the function behind the "relaxed DRC" column of its dashboard, from
tscircuit-autorouter `5bcf774` (`lib/testing/`) with `@tscircuit/checks` 0.0.233, called as its benchmark calls it
(`scripts/benchmark/benchmark-run-task.ts`): 0.1 mm between copper of different nets, 0.1 mm between via holes
(same net included), traces continuous and inside the board. `scripts/fetch_fixtures.sh tscircuit-checker`
installs it (node, packages pinned to the versions that repository names). A board passes when every connection
is routed and the checker reports nothing. The checker was not re-implemented.

tscircuit's dashboard counts boards: "completed" is the share of problems its router solved outright, comparable
with *fully routed* below and not with mean completion; "relaxed DRC pass" is the share solved with no checker
error. Its figures, read on 2026-10-10 for its router at 1× effort: `srj19` 91.5 % completed, 89.5 % passing;
`srj20` 77 % and 77 %. They were not reproduced here: tscircuit's router was not run on this machine, and its run
time per problem is not known to us.

**Results.** `python3 bench/run.py --set <set> --work <N> --jobs 12 --threads 1 --route-args "--variants 1
--no-gpu"`, then `python3 bench/srj_score.py <run>`. All 200 boards of each set, defaults otherwise.

| Set | Work | Mean completion | Fully routed | Clean pass (KiCad) | Relaxed pass (tscircuit's checker) | Median s | p95 s |
|---|--:|--:|--:|--:|--:|--:|--:|
| srj19 | 1 M | 75.7 % | 23.0 % | 23.0 % | 23.0 % | 1.5 | 2.2 |
| srj19 | 10 M | 92.5 % | 60.5 % | 60.5 % | 60.0 % | 9.2 | 14.1 |
| srj19 | 50 M | 96.7 % | 79.5 % | 79.5 % | 78.5 % | 34.2 | 50.2 |
| srj19, tscircuit's router (its dashboard) | | | 91.5 % | | 89.5 % | | |
| srj20 | 1 M | 53.1 % | 9.5 % | 9.5 % | 9.5 % | 1.4 | 1.9 |
| srj20 | 10 M | 81.3 % | 33.5 % | 33.5 % | 33.5 % | 9.1 | 10.9 |
| srj20 | 50 M | 87.6 % | 50.0 % | 50.0 % | 50.0 % | 40.9 | 51.9 |
| srj20, tscircuit's router (its dashboard) | | | 77 % | | 77 % | | |

10 M is the sets' standard budget (the trend rows show how much the figure depends on it). Seconds are the
route's own wall time on one thread with twelve boards running at once on a machine shared with other jobs:
indicative only.

- TraceMaker is behind tscircuit's router on both sets at every budget tried: by 12 points of fully routed
  boards on `srj19` and 27 on `srj20` at 50 M.
- No run added a KiCad DRC error on any board (26 runs, 2,500 board routes): every fully routed board is a clean
  pass. What is missing is completion.
- KiCad and tscircuit's checker agree except on 1 board at 10 M and 2 at 50 M (`srj19`), which KiCad passes and
  the checker fails with "overlaps … (accidental contact)" between two traces. An independent distance
  calculation on the exported traces puts the copper of each reported pair at least 0.21 mm apart. The checker
  (`checkEachPcbTraceNonOverlapping`) also reports two separate collinear segments of different nets as touching
  at some spacings (reproduced with two-segment inputs), which a lattice router's output contains. The relaxed
  column is the checker's verdict as it is.
- TraceMaker's own count of routed connections is sometimes one lower than KiCad's (it joins tracks at their end
  points; KiCad 10 also joins copper that overlaps). The harness uses KiCad's count, and the export follows
  KiCad's rule; the two agreed on every board.

**Budget and portfolio**, first 50 problems of each set (fully routed = clean pass in every row):

| | srj19 | srj20 |
|---|--:|--:|
| 1 M, one variant | 22 % | 8 % |
| 10 M, one variant | 66 % | 34 % |
| 50 M, one variant | 88 % | 46 % |
| 200 M, one variant | 88 % (median 30 s, p95 172 s) | 56 % (median 151 s, p95 195 s) |
| 10 M, default portfolio (8 variants, each with the whole budget) | 88 % (median 143 s on one thread) | 48 % (median 135 s) |

`srj19` stops improving between 50 M and 200 M; `srj20` is still rising. The default portfolio at 10 M does as
well as one variant at 50 M for more total work.

**Escape options**, 10 M, one variant, first 50 problems:

| Option | srj19 fully routed (mean completion) | srj20 fully routed (mean completion) |
|---|--:|--:|
| none | 66 % (92.4 %) | 34 % (82.4 %) |
| `--escape-plan` | 60 % (93.0 %) | 32 % (81.4 %) |
| `--escape-flow` | 60 % (92.2 %) | 28 % (79.1 %) |
| `--escape-plan --via-in-pad` | 60 % (93.0 %) | 32 % (81.4 %) |
| `--via-in-pad` | byte-identical to none, 50/50 boards | byte-identical, 50/50 |
| `--blind-vias`, `--micro-vias` | byte-identical to none, 50/50 | byte-identical, 50/50 |

None helps. The escape plans route a few more connections on `srj19` but finish fewer boards on both sets. The
problems allow neither blind nor micro vias, so those two options correctly do nothing. `--via-in-pad` (which the
problems do not allow either: `allowViaInPad` is absent) changes no board, also not at 0.15 mm clearance where
`tracemaker escape` reports dead balls; why it finds no pad to use was not investigated. The router was not
tuned in this work.

**Stricter variant**: the importer's defaults (`bench/prepare_srj.py --clearance-mm default --hole-to-hole-mm
default`: 0.15 mm clearance, no hole-to-hole rule), 10 M, one variant, all 200 boards.

| Set | Mean completion | Fully routed = clean pass | Relaxed pass |
|---|--:|--:|--:|
| srj19 | 81.8 % | 14.5 % | 10.0 % |
| srj20 | 78.2 % | 29.0 % | 29.0 % |

At 0.15 mm a 0.15 mm trace does not fit between two balls of `srj19` (0.44 mm gap), so inner balls can leave
only through vias; `srj20`'s 0.12 mm traces still fit. The 9 `srj19` boards that are clean in KiCad and fail the
relaxed check have vias of one net closer than 0.1 mm hole to hole, which KiCad does not test without a
hole-to-hole rule: the reason the benchmark sets carry that rule.

**Limits of the comparison.** The judges differ (KiCad's DRC against tscircuit's checker; here they agree to
within 1 % of boards). The rules are an interpretation where the files are silent (clearance, hole spacing, via
size for `srj20`; no board-edge clearance, which tscircuit's relaxed check does not apply either). The budgets are not matched to
tscircuit's effort setting. The problems are synthetic and two-layer only.

## 8. M14: srj19/srj20 against tscircuit's current run, and fanout31 (2026-10-11)

tscircuit's dashboard run `38105514608-1` (commit `65522c8`): srj19 92.0 % completed / 90.0 % relaxed pass, srj20 78.5 % / 78.5 %
(360 s per board, 32 vCPU; median 19–23 s). TraceMaker, one variant, 50 M work, `--fast-bends` and the new default limits (40 rips, 40
passes): **srj19 176/200 = 88.0 %, srj20 117/200 = 58.5 %** (section 7: 79.5 % and 50.0 %). With the default eight-variant portfolio on the
boards that miss: srj19 +10, srj20 +9 (about 93 % and 63 %). Per-board comparison with the dashboard run, the gap sets
(`srj19-gap`, `srj20-gap`, `srj19-p1fail`, `srj20-p1fail`) and every experiment: doc 16 §10. `bench/fanout31_run.py` scores the 72 fan-out cases
(solved = every connection routed, no unconnected item after the zone refill); results in doc 16 §10.1.

Update, same day (doc 16 §11): with preferred layer directions and the package penalty switched on automatically for breakout boards, one variant at 50 M
gives **srj19 181/200 (90.5 %)** and **srj20 124/200 (62 %)**; the 16-variant portfolio (50 M each, 16 threads, median 164–168 s) closes 7 more on
srj19 (**188/200 = 94.0 %**, tscircuit 92.0 %) and 9 more on srj20 (**133/200 = 66.5 %**, tscircuit 78.5 %). `fanout31`: 57 of the 60 cases run solved
(tscircuit 48/72); AM3352 is not solved (6–23 connections open at best; tscircuit solves none of its 12 cases either).

## 9. M15: negotiation as transactions (2026-10-11 evening, doc 16 §12, doc 05 §41)

The breakout profile (D100: transactions, reach-check 2, flood weight 25 %, field prefill and cache) is automatic on breakout boards, so the commands of §8
need no extra options and their results change: one variant at 30 M work gives **srj19 178/200** (was 181 at 50 M, 46 → 32 s CPU per board) and
**srj20 129/200** (was 124 at 50 M, 67 → 59 s). The portfolio is the breakout portfolio with four rounds (D101):

```
python3 bench/run.py --set srj20 --work 30000000 --route-args "--variants 1 --no-gpu --fast-bends"
python3 bench/run.py --set srj20-m15fail --work 30000000 --jobs 3 --threads 16 --route-args "--variants 16 --no-gpu"        # 71 boards: +19
python3 bench/run.py --set srj20-m15fail3 --work 70000000 --time 330 --jobs 3 --threads 16 --route-args "--variants 16 --no-gpu"   # 24 boards: +8
```

Sets added: `srj20-p2fail` (76: the boards P2 leaves open), `srj20-e39` (39 of them), `srj20-m15fail` (71), `srj20-m15fail2` (52), `srj20-m15fail3` (24 of
fewer than 70 connections), `srj20-m15fail4` (16), `srj19-m15fail` (22), `srj20-rem67`. Results: srj19 **191/200 = 95.5 %** (178 + 13), srj20 **160/200 = 80.0 %** (156 within 330 s)
(129 + 19 + 8 + 4 with GPUs and 150 M, up to 433 s), tscircuit 184 and 157. These are composites of single-variant and portfolio runs (the wall limit of the last step makes it
non-deterministic: 330 s, 16 threads; tscircuit's 360 s, 32 vCPU). `quick` and `mid` are byte-identical to the baseline outputs.

Work-budget note: on a breakout board a reachability-flood cell counts a quarter of an expansion against `--work`, so `--work` budgets from §7–§8 are not comparable
with these (30 M here is about 100–140 s of CPU per hard board; 50 M plain was 67–115 s).

## 10. Advice (M16, doc 17 §8)

`bench/advice_eval.py --set SET --name RUN --work W --variants V --advisor CMD [--half even|odd|all]` routes each board with
`route --advisor CMD`: a plain portfolio, then, if connections are left, the advisor and a portfolio with its advice (the unadvised variants
again plus advised ones); the better result stands. `bench/results/RUN/advice.json` has one row per board (connections routed by the plain run,
by the advised run, whether it was kept, the best advised variant) and a summary; the stuck report and advice of each board are stored beside
it, so a result can be reproduced with `route --advice FILE`. The gate of issue #26 stage 2 is read on the odd half of a set, the even half
being where an advisor is tuned.


## 11. M17: PCBWorld-Bench's real boards and the failing tier B-D boards (2026-10-16, D121, D122)

Records: `bench/records/m17/` (README with all tables; raw `boards.jsonl` of every run).

### 11.1 What PCBWorld-Bench's "679 real boards" are

Doc 18 (A1) took PCBWorld's D3 for an independent second corpus of real boards. Reading the paper (arXiv 2607.05915, the PDF is in the repository clone) and its data
tools shows it is not: **D3 is derived from PCBench**, the corpus this repository already benchmarks. The authors convert PCBench boards to KiCad 9 (design rules kept, then
`drc_fix_v9.py` lowers severities to *ignore* and a few rule floors to 0), keep the 679 whose converted human layout is fully connected and has no violation, build a "guide
board" with one uniform width per net, and split the pool by pad count into D3-A (100 smallest boards), D3-B (287) and D3-C (292). Their evaluation uses fixed test lists
(`configs/datasets/d3.json`): **99 D3-A boards, 10 D3-B, 10 D3-C**, and publishes results for D3-A and D3-B only (D3-C is "reserved for future research"). All 119 test
boards are PCBench fixtures here by name (`bench/sets/pcbworld-d3-test.txt`). The PCBWorld repository holds no boards and its conversion needs a KiCad 9 `pcbnew` built from
their engine; that conversion was not reproduced.

What D3 gives, then, is a fixed, published, pad-count-ordered subset with published baselines, not new boards.

### 11.2 TraceMaker on the 119 test boards

`TM_BINARY=build/dist/src/app/tracemaker python3 bench/run.py --set pcbworld-d3-test --time 120 --jobs 3 --threads 8 --name m17-pcbworld-d3` (the release binary, default
eight-variant portfolio, KiCad 10.0.6 judge: every connection routed and no error added against the input). `bench/pcbworld_d3.py` prints the tables.

| class | boards | TraceMaker clean | mean routability | boards open | median s | Freerouting 2.5.0-RC12 clean (PCBench nightly) |
|---|---:|---:|---:|---:|---:|---:|
| D3-A | 99 | **97** (98.0 %) | 0.994 | 2 | 1.3 | 94 (94.9 %) |
| D3-B | 10 | **10** (100 %) | 1.000 | 0 | 10.0 | 9 (90.0 %) |
| D3-C | 10 | **9** (90.0 %) | 0.996 | 1 | 61.4 | 8 (80.0 %) |

The same boards with Freerouting **2.1.0 and the paper's settings** (`-mp 10`, 4 GiB heap, the jar's other defaults; the paper's Table 20), three runs per board from the
fixture's DSN, session imported (`bench/ses_import.py`) and judged by the same judge (`bench/pcbworld_fr.py`; a run over 600 s counts as a failure: 10 of 357 runs, on four boards):

| class | Freerouting 2.1.0 clean, mean of 3 runs | clean in any of 3 runs | mean routability | TraceMaker clean, Freerouting not | Freerouting clean, TraceMaker not |
|---|---:|---:|---:|---:|---:|
| D3-A | 0.670 | 73 (73.7 %) | 0.978 | 24 | 0 |
| D3-B | 0.500 | 5 (50.0 %) | 0.992 | 5 | 0 |
| D3-C | 0.200 | 3 (30.0 %) | 0.758 | 6 | 0 |

**Boards lost.** To Freerouting 2.1.0 (this judge): none. To the 2.5.0-RC12 nightly flag: one, D3-A `AVR-ISP_pogo-plug_1.27mm_AVR-ISP_pogo-plug_1.27mm`, where 3 of 6 connections stay
open ("boxed in"; the footprint is a row of 0.4 mm strip pads 0.15 to 0.3 mm apart; its stuck report names fixed copper around the pin; no cause beyond that was found). The other
two boards TraceMaker leaves open, D3-A `memsarray_mems_modules` (9 of 10 routed) and D3-C `kitspace_f-91w` (80 of 83, the 120 s limit), are not clean in the nightly run either, and
Freerouting 2.1.0 hit the 600 s limit on both in all three runs. Against the paper's other methods (KiCadRoutingTools, OrthoRoute, the agents and the RL policies) no per-board
result is published, so no board can be named.

**What these numbers do not say.** The paper's Table 3 (Freerouting 2.1.0 0.80 / 0.78 clean pass on D3-A / D3-B, KiCadRoutingTools 0.74 / 0.20, PPO 0.86 / 0.45, GPT-5.4 0.65 / 0.00;
best of five rollouts selected by the paper's potential function) comes from a different judge: KiCad 9, the converted boards with their patched rule files, absolute violations.
Freerouting 2.1.0 with the same jar and settings scores 0.670 at one run and 73.7 % over three runs under this repository's judge, against the paper's 0.75 at one run and 0.80 over
five, so this judge is not the paper's and the paper's rows for the other methods cannot be set beside TraceMaker's. The only like-for-like comparison here is TraceMaker against Freerouting
2.1.0 under one judge, from one DSN, and it is not the paper's DSN. There is no "better than the paper's methods" claim. D3 test boards are small (median 1.3 s); tier D of this
repository is the harder test, and doc 18 §8's expectation that A1 "could look good and teach little" holds.

### 11.3 `--txn` and `--relax` on the failing tier B-D boards (C1)

The 33 boards of tiers B, C and D that the 0.9.0 run left open (`bench/sets/pcbench-bd-fail.txt`; 16,935 connections; none added an error). Same work for every arm; KiCad-judged.

| arm (100 M work per variant) | routed | boards better / worse than the base | clean boards | CPU |
|---|---:|---:|---:|---:|
| one variant, base | 15,289 | | 0 | 4,858 s |
| one variant, `--txn 1` | 15,320 (+31) | 7 / 0 | 0 | 3,924 s |
| one variant, `--relax` (5 rounds of 20 M) | 14,206 (-1,083) | 3 / 28 | 0 | 4,644 s |
| eight variants, base | 16,105 | | 0 | 77,014 s |
| eight variants, `--txn 1` | 16,223 (+118) | 15 / 4 (by one connection each) | 0 | 96,039 s |
| eight variants, `--relax` (5 rounds of 20 M) | 15,596 (-509) | 11 / 16 | **2** | 106,470 s |

- **`--txn 1`** (negotiation as transactions, automatic on breakout boards only) routes more connections on non-breakout boards in both settings: up to +16 on `mechkeys_LFK78`, never more than one fewer on a board.
  It completes no board that the base leaves open, so clean pass does not move. It costs more CPU for the same work in the portfolio (+25 %).
- **`--relax`** is a rescue, not a better default: it loses connections on most of these boards (down to -107 on `kitspace_d20_tri_r0.3`) and finishes `kitspace_ardfpga` and
  `Hardware_Playground_multisensor_cr2032` with no error added, which nothing else did. Taking the better of base and relax per board gives 16,148 routed (+43) and the two clean boards.
- **Bug found by the experiment:** `--relax` (and `--rounds`, `--round-work`, `--ramp-from`) did nothing with `--variants 1`; the first relax arm was identical to the base on all 33 boards
  (`bench/records/m17/c1-relax-ignored.md`). They now run a portfolio of one (`tests/integration/round_options.py`).
- **Not concluded:** whether `--txn 1` as one or two of the eight default variants beats the current default at equal wall time on tiers A to D, which is what changing the default needs;
  whether a relax pass after a plain run that leaves connections open pays for its time. One seed, one budget, and `mini_mass_prog_bench_prog_rig` hit the one-hour safety stop in the `--txn` and
  `--relax` arms. Records and caveats: `bench/records/m17/README.md`.

Quick tier with the release binary: 30 of 30 routed boards byte-identical to main's.
