# Overnight report: M9 escape planning and M13 component-aware rules

Written during the unattended run of 2026-10-04/05. Everything is committed locally on `main` (not pushed).
Assumptions taken without being able to ask are listed at the end (M9: `dev/assumptions.md` A26–A31; M13:
`dev/assumptions-m13.md`).

## M9 — escape planning

### What the BGA boards were really failing on

The 17-board BGA/dense-package set (`bga-base`, 41 % clean) fails for two different reasons, both reported as
"boxed in":

1. **Infeasible under the rules the benchmark judges with.** PCBench ships boards without their `.kicad_pro`, so
   KiCad's defaults apply (0.25 mm track, 0.2 mm clearance, 0.8/0.4 mm via), and KiCad 5 boards keep a large
   `pad_to_mask_clearance` with untented vias. Under those rules the designers' own routed boards fail KiCad DRC by
   hundreds of errors (OtterCast: clearance, track width, via diameter; decelerator4030: 499 clearance). No router can
   route those pins cleanly — Freerouting fails the same boards.
2. **Feasible, but the channel was taken.** On large boards the strict first pass uses the whole budget, and pins whose
   exit another net took first stay boxed in because negotiation never starts.

### What was built

- **`tracemaker escape <board>`** — escape feasibility per dense package: which pins cannot leave the package at all
  under the board's rules, why, and what would fix it (e.g. sbc: "22 DRAM balls: only the solder-mask rule blocks via
  sites — tent the vias or reduce pad_to_mask_clearance (0.2 mm)"). 0.1–2 s per board. Over all 1,157 PCBench boards:
  42 boards have pins that cannot escape (789 of 46,628 dense-package pins); in the benchmark tiers 2 of 45 (B) and
  4 of 39 (C). `bench/run.py` now records `dead_pins` and a `clean_pass_feasible`; `bench/feasibility.py` splits
  finished runs.
- **Escape corridors (`--escape-plan`)** — every pin of a dense package gets a reserved corridor (out of the package
  for perimeter pins, a dog-bone to its via site for inner balls); other nets may not take it in the strict pass.
  One variant, same budget: logicbone 938 → 964, decelerator 446 → 479. BGA set (8 variants, 120 s): 6,916 → 6,947
  routed, no added errors.
- **Via neck-down** (default) — when the class via does not fit, the escalation rung uses the smallest via the board
  minimums allow (KiCad checks vias against those). d20_tri 164 → 188, OtterCast 160 → 177, 0 added KiCad errors.
- **Dead pins** (default) — a pin still boxed in by a negotiated search at the neck-down sizes is enclosed by fixed
  copper; it is reported as such and not retried.
- Tests (`tests/test_escape.cpp`), `debug-pad --via` (via legality map), doc 05 §12.

### Regression runs (CLAUDE.md rule 9)

Same machine, 120 s per board, 8 variants, 2 boards at a time. "cur" = tonight's router without escape planning
(via neck-down and dead pins are in), "all on" = escape planning in all 8 variants, "arm" = the default now
(escape planning in 2 of the 8 variants). No run added a single KiCad DRC error.

| Set | final10 (before) | cur | all on | arm (default) |
|---|---|---|---|---|
| Tier A (40) | 100 % clean | 100 % | 100 % | 100 % |
| Tier B (40) | 67.5 %, 5,126 routed | 67.5 %, 5,163 | 67.5 %, 5,171 | 67.5 %, 5,166 |
| Tier C (30) | 60.0 %, 9,503 routed | **63.3 %**, 9,521 | 63.3 %, 9,514 | 63.3 %, 9,526 |
| Tier D (22) | 50.0 %, 16,497 routed, 95.0 % completion | – | – | 50.0 %, **16,626**, 96.2 % |
| BGA set (17) | 41.2 %, 6,916 routed | – | 41.2 %, 6,947 | – |

Tier C gained a board (multisensor_cr2032) from the via neck-down / dead-pin changes. In tier D, Keyboard became
clean and m2fc lost its clean pass by one connection (723/724, "search budget"): the old binary's winning variant on
m2fc was one of the two that now carry escape planning. At a fixed 70 M budget old and new route m2fc identically
(707) and escape planning routes 709, so this is the last connection falling either side of the 120 s limit, not a
broken board. Clean pass on the boards
whose pins can all escape: tier B 69.2 % (39 boards), tier C 70.4 % (27 boards), BGA set 46.7 % (15 boards).

### Not done (rest of M9)

Min-cost-flow channel assignment for deep arrays, per-ring layer assignment, escape templates in the knowledge base.
Second-ring channel corridors are built and tested but measured mixed, so they are off (`--escape-second-ring`).

## M13 — component-aware rules

Built by a helper agent on its own branch, reviewed and merged (`c058336`); all 75 tests pass after the merge.
Everything is **off by default** because the measurements showed regressions (D31).

- **Catalogue → engine:** `docs/component_rules.yaml` is converted to an embedded JSON (a test keeps YAML, JSON and
  doc 15 in sync); new module `src/crules/` detects categories with confidence, binds roles (USB D+/D−, ESD part,
  crystal + load caps + oscillator pins, regulator caps and inductor, CAN/RS-485, antenna keep-out) and reports every
  rule as applied / satisfied by the board / not applied (with the reason) / advisory.
- **`tracemaker rules <board>`** prints that report (and JSON, and a sidecar `.kicad_dru` that KiCad 10 parses).
- **Detection quality** on 12 held-out labelled boards: precision 1.00 for every category except CAN (0.50, two LIN
  transceivers, pattern narrowed since); recall 0.58–1.00 (USB 0.63, connectors 0.58, mounting holes 0.71 are below
  the 0.8 gate: vendor part-number footprint names). All PCBench boards are KiCad 5 files without pin names, so most
  interface categories are capped at confidence 60 (suggest), below the apply threshold.
- **Placement (`tracemaker-place --component-rules soft`):** crystals, load caps and ESD parts get closer at equal
  wirelength (crystal 6.4 → 5.1 mm, ESD 7.1 → 3.2 mm on 9 ESD boards) but decoupling and regulator caps get worse on
  some boards because the new pulls take the space next to the IC. I added `--crules-weight` to scale them: at 50 %
  decaps and regulator caps beat even "off" (3.66 → 3.33 mm, 4.25 → 3.79 mm) but the crystal gain disappears. Only
  ~4 of 23 boards change at all, so I left the default at 100 % and the feature off.
- **Routing (`tracemaker route --component-rules soft|on`):** USB D+/D− routed as coupled pairs (soft; usually falls
  back to single tracks on these short runs) and crystal/inductor keep-outs (on). Keep-outs cost completion on 4 of
  10 affected boards (worst 93 → 86 connections), so they stay opt-in.
- **P4 (stackup + impedance):** built by a second helper agent and merged (93 tests pass). The board stackup is now
  read (layers, thickness, εr, loss tangent, sublayers; read-only, files still round-trip byte for byte) and a cited
  closed-form solver computes microstrip (Hammerstad & Jensen), stripline (Cohn / Wheeler), edge-coupled pairs
  (Kirschning & Jansen, Cohn) and grounded coplanar lines, plus IPC-2221 width for current. Checked against KiCad's own
  calculator code: identical for stripline and coupled microstrip, 0.2–2.8 % for microstrip (KiCad adds a thickness
  term), and against Pozar's textbook examples within 0.2 %. `tracemaker rules` now reports e.g. "USB2-01 90 Ω diff on
  F.Cu: 0.201 mm / gap 0.250 mm microstrip (h 0.100 mm, εr 4.50), 6.06 ps/mm, formula error ±5 %". **Report only**:
  routing widths are unchanged (D36). None of the 1,157 PCBench boards has a stackup (KiCad 5 exports), so on the
  benchmark impedance rules always say "not applied: no stackup in the board"; 35 other boards (KiCad demos, 8 raw
  PCBench originals, Freerouting issue fixtures) were used for testing.

## Assumptions

See `dev/assumptions.md` (A26–A31, M9) and `dev/assumptions-m13.md` (M13). The ones most worth a look:

- **A27/D33 — escape planning runs in 2 of the 8 portfolio variants**, chosen without data on which variants win
  (the route JSON now records the winner, so this can be revisited).
- **A29/D34 — via neck-down:** the router may now use the smallest via the board minimums allow (0.5/0.3 mm on
  boards without a project file instead of the class 0.8/0.4 mm), only when the class via does not fit. This is a
  manufacturable size, but it is a change in what the router produces.
- **A31/D35 — "infeasible" is defined against the rules the benchmark judges with**, which are KiCad defaults for
  PCBench (no `.kicad_pro`). Several "failures" are not failures of any router; the clean pass is still reported in
  full, with the feasible split beside it.
- **M13 (dev/assumptions-m13.md):** keep-outs only in `on` mode (doc 15 says soft adds no hard constraints); keep-outs
  only on layers where the parts have no pads and only for tracks (KiCad rule areas cannot exempt a part's own nets;
  the full rule is in the sidecar `.kicad_dru`); detector patterns in the catalogue were edited (library nicknames
  ignored, buck/boost part-number weights 30 → 50); HV-02 demoted to soft because its evidence is unverified.
