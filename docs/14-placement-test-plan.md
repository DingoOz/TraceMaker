# 14 — Placement test plan

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Design: [04-placement.md](04-placement.md).
> Gates: [11-roadmap.md](11-roadmap.md) M7 and M8. Written 2026-10-03.

This plan says how component placement (`tracemaker-place`) is tested: what is checked, on which boards, against
which baselines, and what has to be true before placement is called good. It covers the existing modes
(`full`, `refine`, `auto` with `--route-check`) and the M8 additions (`routable`, `eco`).

## 1. What "good placement" has to mean

A placement is good when, in this order:

1. **It is legal.** No new courtyard overlaps, parts inside the board outline, pads keep copper, edge and
   solder-mask clearances, locked and fixed parts untouched, and the board still opens in KiCad unchanged apart
   from the moved footprints.
2. **It routes at least as well as the human placement.** Measured by TraceMaker and Freerouting, judged by
   KiCad's DRC, with the same budget for both placements.
3. **It respects design intent that the netlist does not state.** Decoupling capacitors stay close to their
   chip, connectors stay at the edge, crystals stay close to their oscillator pins, mechanical parts stay put.
4. **It is short.** Wirelength (HPWL) and airwire crossings go down, with a reported distance from the proven
   lower bound.
5. **It is reproducible and reasonably fast.** The same seed gives the same board at any thread count, and
   placement time is recorded.

Wirelength comes fourth on purpose: the M7 evaluation showed that shorter wirelength from denser packing can make
a board route worse (13 fully routed boards against 15 for the human placement), so wirelength alone never
decides that a placement is better.

## 2. Test levels

| Level | What | Where | Runs |
|---|---|---|---|
| L0 unit | Geometry, legality, cost, solvers, determinism | `src/place/test_place.cpp` (Catch2, `ctest -L place`) | Every build |
| L1 property | Invariants on random and fixture inputs (below) | `test_place.cpp`, tagged `[property]` | Every build |
| L2 board | Place real boards, check legality and the KiCad round trip | `src/place/eval_place.py --truth` | Every placement change |
| L3 routability | Route human vs placed boards with a fixed budget; KiCad DRC | `bench/place_auto.py`, `bench/place_bench.py` (to build, §7) | Every placement change (quick set); weekly (full set) |
| L4 baselines | SA-PCB and other placers on the same boards | `bench/place_baselines.py` (to build) | Per milestone |
| L5 human review | A designer looks at placed boards in KiCad | Checklist (§6) | Per milestone |

### 2.1 Unit tests (L0) — existing

The 11 tests in `test_place.cpp`: KiCad rotation convention; convex hull; power-net name detection; translated
`closer()` agreement with the exact geometry predicate; min-cost flow on a small graph; the HPWL lower bound is
exact on a chain and bounds random placements; the raster fast path is conservative (raster free implies exactly
legal); legalisation removes every overlap; the full pipeline is legal, deterministic and its incremental cost is
exact; rotation descent is monotone; problem extraction from a KiCad board.

### 2.2 Property tests (L1) — to add

Each property is checked on 50 seeded random problems and on 5 fixture boards:

| # | Property |
|---|---|
| P1 | Every output placement is legal under the exact checker (not only the raster). |
| P2 | Locked parts, fixed parts (connectors at the edge, mounting holes, parts with Edge.Cuts or keepouts) and parts touching existing routed copper keep their position, rotation and side exactly. |
| P3 | The reported HPWL equals an independent recomputation from the written board. |
| P4 | Lower bound ≤ HPWL of every placement found (human, full, refine). |
| P5 | Same seed ⇒ byte-identical output board for 1, 3 and 16 threads. |
| P6 | `refine` never increases the cost it optimises; LNS and parallel tempering never return a worse best state than their start. |
| P7 | `--route-check` and `auto` never return a placement with more unrouted connections than the input (same work budget). |
| P8 | `eco` moves at most the configured number of parts and only parts within the configured distance of failed connections. |
| P9 | Placing an already optimal toy problem (two parts, one net) returns the analytic optimum. |

## 3. Datasets

| Set | Boards | Purpose | Notes |
|---|---|---|---|
| **Q — quick** | 10 PCBench tier-A/B boards (fixed list below) | Every placement change | 2–4 minutes on 16 threads |
| **D — development** | The 23 boards of doc 04 §7.3 | Tuning | Already used: never quote results from it as final |
| **H — held-out** | 40 PCBench boards never used in placement or routing development (seeded sample, §3.1) | Milestone results | Frozen binary; no tuning afterwards |
| **DAC** | DAC 2020 benchmarks (`bench/data/dac2020_prepared`, 10 boards with Specctra files, 11 in total) | Placement benchmark built for this purpose | Locked parts are marked in the files; the authors' metrics are via count, layers and bounding-box area |
| **S — from scratch** | The H boards with every movable part piled at the board centre (generated) | `full` mode as a schematic-to-board flow; the M8 gate's "schematic-to-board set" | Human placement still serves as the reference |
| **K — KiCad demos** | Demo boards with movable parts | Rule coverage (net classes, custom rules, zones) | Not a quality benchmark |

Quick set Q: `ChirpHardware_chirp`, `IGN01A_IGN01A`, `LadybugLiteBlue_HW_LadybugBlueLite`, `PiPlay_SDHat`,
`Solare-BQ24210_Solare-BQ24210`, `beast-phat_beast-phat`, `domotics_out-board`, `kitspace_hbridge_driver`,
`scimpy_volumebuffer`, `uC3Moy_uC3Moy`.

### 3.1 Selecting the held-out set H

Sample with a fixed seed (`7`) from PCBench boards that:

- appear in no `bench/results/*/boards.jsonl`, no `quality.jsonl` and none of the D boards;
- have at least 10 movable parts and at most 300 parts;
- have a courtyard on at least 80% of their parts (otherwise legality is undefined);
- route completely in their human placement with TraceMaker at a 120 s budget (so routability changes are
  attributable to placement).

Stratify 20/20 by part count (below and above the median). Record the list in `bench/place_sets/H.txt`
and never change it; a new held-out set gets a new name.

## 4. Metrics

All metrics are computed from the written `.kicad_pcb`, never from the placer's internal state.

| Group | Metric | Definition |
|---|---|---|
| Legality | New KiCad DRC errors | `courtyards_overlap`, `pth_inside_courtyard`, `npth_inside_courtyard`, `copper_edge_clearance`, `clearance`, `shorting_items`, `solder_mask_bridge`, `hole_clearance` errors in the placed board that the human board does not have |
| Legality | Round trip | `kicad_truth.py` on the placed board equals `tracemaker inspect --json` except for moved footprints |
| Legality | Fixed parts | Count of locked or fixed parts whose position, rotation or side changed (must be 0) |
| Routability | Unrouted (TraceMaker) | Unrouted connections after `tracemaker route --work W --threads 8`, W fixed per board size, and at 120 s |
| Routability | Unrouted (Freerouting) | Same with Freerouting 2.5 and 1.9 via `bench/compare.py` (independent router: guards against a placement tuned to TraceMaker) |
| Routability | Clean pass | Fully routed and no router-introduced KiCad DRC errors |
| Routability | Route quality | Track length, vias, bends (`bench/quality.py`) of the routed placed board vs the routed human board |
| Intent | Decoupling distance | For each capacitor whose nets are one power and one ground net shared with an IC, the distance to the nearest such IC's power pin; report median and maximum, human vs placed |
| Intent | Connector edge | Connectors (reference prefix J/P/CN or footprint library "Connector") within 2 mm of the edge, human vs placed (must not drop) |
| Intent | Crystal distance | Crystal/oscillator to the IC pins it connects to (median, maximum) |
| Intent | Orientation | Share of two-pin passives at 0°/90°/180°/270°; mixed 45° rotations count as violations |
| Wirelength | HPWL | Signal HPWL, weighted HPWL (power nets ×0.1) |
| Wirelength | Lower-bound gap | Weighted HPWL ÷ the min-cost-flow lower bound (any rotation) |
| Wirelength | Crossings | Airwire (MST) crossings |
| Wirelength | Area | Bounding box of all parts (the DAC 2020 metric) |
| Process | Time, determinism | Placement wall time and moves; output hash for 1 and 16 threads |

## 5. Baselines and comparisons

| Baseline | How | Why |
|---|---|---|
| Human placement | The fixture as given | The real target: a designer accepted it |
| TraceMaker `refine`, `full`, `auto`, `routable`, `eco` | `tracemaker-place --mode ...` | Mode-against-mode |
| SA-PCB | Build The-OpenROAD-Project/SA-PCB (BSD-3, archived 2026) under `build/tools/`, adapter from `.kicad_pcb` | The M7 gate: HPWL ≤ SA-PCB |
| Random legal placement | `full` with the annealer off and a random start (seeded) | Sanity floor: every metric must beat it |
| Ablations | `full` without rotations, without crossings term, without the routability term | Each part of the cost must help or be removed |

## 6. Human review checklist (L5)

For 5 boards per milestone (2 from H, 2 from DAC, 1 from S), a designer opens the human and placed boards side by
side in KiCad and scores 1–5:

1. Would you manufacture this placement without changes?
2. Decoupling capacitors next to their pins?
3. Connectors, switches, LEDs and mounting holes where the enclosure expects them?
4. Silkscreen readable (references not under parts)?
5. Thermal parts (regulators, power transistors) spread and near copper?
6. Analogue/digital and high-current/small-signal separation kept?

Record the scores and comments in `dev/placement_review.md`. A score below 3 on question 1 for any board is a
release blocker for that mode, whatever the numbers say.

## 7. Tooling to build

| Tool | Purpose |
|---|---|
| `bench/place_bench.py` | One command for L3: place (all modes) → route with TraceMaker (work budget and 120 s) and optionally Freerouting → KiCad DRC → metrics of §4 → `bench/results/<run>/place_summary.json` and `report.md`; reuses `eval_place.py`, `quality.py`, `compare.py` |
| `bench/place_sets/` | Frozen board lists Q, D, H, DAC, S |
| `bench/make_scratch_set.py` | Builds set S: movable parts piled at the board centre, everything else unchanged |
| `bench/place_intent.py` | Design-intent metrics (decoupling, connector edge, crystal, orientation) |
| `bench/place_baselines.py` | SA-PCB build, adapter and runs; random-placement floor |
| Dashboard panel | Latest placement run: legality, unrouted vs human, HPWL ratio, intent metrics |

## 8. Gates and pass criteria

| Gate | Criterion | Set |
|---|---|---|
| G1 legality (every change) | 0 new KiCad DRC errors of the legality group; 0 moved fixed parts; round trip exact; P1–P9 pass | Q (and H at milestones) |
| G2 routability (every change) | Unrouted (TraceMaker, fixed work) summed over Q not above the human placement; no board worse by more than 1 connection in `auto` | Q |
| G3 M7 gate | HPWL ≤ SA-PCB on ≥ 80% of H boards; G1 on H | H |
| G4 M8 gate | Clean pass ≥ 90% of S boards with `routable`/`full` (TraceMaker, 120 s); `eco` closes at least half of the H boards that `none` mode leaves unrouted | S, H |
| G5 independence | On H, the placed boards' unrouted count with Freerouting 2.5 is not above the human boards' | H |
| G6 intent | Median decoupling distance and connector-at-edge count not worse than human on H | H |
| G7 human review | No board scored below 3 on question 1 | L5 boards |

A change that fails G1 or G2 is not merged. G3–G7 are reported with every milestone result, pass or fail.

## 9. Procedure

1. `ctest --preset release -L place` (L0, L1).
2. `bench/place_bench.py --set Q --modes refine auto --work 3000000` (L2, L3) on every placement change.
3. At a milestone: freeze the binary, run `--set H`, `--set DAC`, `--set S` with all modes, Freerouting
   comparison on H, baselines (L4), then the human review (L5).
4. Publish: `report/` section and the dashboard panel; record decisions in doc 12.

## 10. Current status (2026-10-03)

| Item | Status |
|---|---|
| L0 unit tests | 11 tests, passing; M8 adds tests for parallel tempering, LNS and ECO (in progress) |
| L1 property tests | Not yet written as such (P5 and P7 are partly covered by existing tests and `place_auto.py`) |
| L2 board evaluation | `eval_place.py` (legality, round trip, HPWL, crossings) on the D set |
| L3 routability | `place_auto.py` on the D set: HPWL −24.5%, unrouted 61 → 59, 0 new DRC errors |
| Held-out set H, scratch set S, DAC placement runs | Not started |
| Design-intent metrics | Not started |
| SA-PCB baseline (M7 gate) | Not started |
| Human review | Not started |
