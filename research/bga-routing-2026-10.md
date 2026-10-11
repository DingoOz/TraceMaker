# Research: BGA breakout and escape routing, and what has been shown not to work (checked 2026-10-11)

Gathered by direct page fetches, the GitHub API (`gh`) and web search. Every claim carries an evidence tag:

- **[V]** verified here: the primary source was fetched and read, or the number was computed from fetched data.
- **[A]** abstract or search-result summary only: the paper's full text was not read (several publishers returned 403
  or a bot check). Treat as a lead, not a measurement.
- **[C]** a calculation made in this note from numbers in a [V] source.

The plan that uses this note is [`docs/16-bga-breakout-plan.md`](../docs/16-bga-breakout-plan.md).

## A. What tscircuit tests against, and what it claims

| Item | Finding | Tag |
|---|---|---|
| Benchmark that claims BGA results | `dataset-srj19` (BGA breakouts, passives on the other side; MIT) and `dataset-srj20` (BGAs with missing balls; no licence stated): 200 problems each, two layers, 0.8 mm pitch, 0.36 mm square pads. Already integrated in this repo on branch `t6-srj` (D90) | [V] |
| Their router's published rate | Dashboard, run `38105514608-1` (commit `65522c8`, 2026-10-11, "1x effort", pipeline `AutoroutingPipelineSolver9_PreloadedTraceGraph`): **srj19 92.0 % completed / 90.0 % relaxed-DRC pass; srj20 78.5 % / 78.5 %**. Per-board 360 s timeout, 32 vCPU ARM runner; median 19.2 s (srj19) and 22.8 s (srj20); 16 and 42 boards time out. The rate moved from 91.5/77 to 92/78.5 within one day: quote the run id, not a bare percentage | [V] |
| Their failure modes | srj19: 8 of 20 failures time out in `highDensityForceImproveSolver`, 5 in `pipeline9JointDrcRepairSolver`, 7 are relaxed-DRC errors (`pcb_trace_error` 4, pad–trace clearance 3, via–trace 2, `trace_smtpad_accidental_contact` 2). srj20: 35 of 43 failures time out in `highDensityForceImproveSolver`. So most of their failures are **search that did not finish**, not proven infeasibility | [V] |
| Where their time goes | Mean per board, srj19: `highDensityRouteSolver` 25.0 s (37 %), `highDensityForceImproveSolver` 18.5 s (27 %), `pipeline9JointDrcRepairSolver` 14.7 s (21 %), global path planning (`portPointPathingSolver`) 1.8 s (2.7 %), topology planning/merging 0.2 s. srj20: route 42.7 s, force-improve 40.4 s, joint repair 15.8 s. The stage list is: escape-via location → topology planning → capacity mesh → port-point (global) path finding → per-cell high-density route → force improve → repair → stitch → simplify → global DRC force improve → joint DRC repair | [V] (computed from the run JSON) |
| Via use | On the 156 srj19 boards both routers solve, tscircuit uses 72.5 vias per board on average, TraceMaker 43.6 (srj20: 56.8 vs 39.6, 100 boards) | [C] from `build/t6-srj/bench/results/*-w50m/boards.jsonl` and the run JSON |
| Second BGA benchmark | `tscircuit/fanout-solver` (MIT) with `dataset-fanout31-am62l`: 72 SoC fan-out cases (AM62L 373 balls 0.5 mm, RK3308 355 balls 0.65 mm, K230 390 balls, i.MX6ULL 289 balls, T113-S3 128 leads, AM3352 324 balls). Committed report (2026-09-09, commit `090a27a`): **solved 48/72, 24 timeouts at 120 s** (AM62L 12/12, K230 12/12, RK3308 9/12, i.MX6ULL 11/12, T113-S3 4/12, AM3352 0/12). A case counts only if every SoC ball (signals and plane drops) is validated. Later commits (to 2026-10-02) fix individual cases; the committed totals were not regenerated | [V] |
| What the fan-out problem is | A prefix problem, not a board: every pad is routed to one **shared breakout edge** and must end exactly on it; plane balls (ground, power) are connections with a `termination: plane` (a via to an inner layer); buses are atomic (one layer, all-or-none vias), with `maxLengthSkew` and differential pairs; 8-layer stackup in the fixtures I read (escape layers `top`, `inner4`–`inner6`, `bottom`; planes on `inner1`); 0.08128 mm tracks, 0.24 mm vias with 0.1 mm hole, through vias only, no via-in-pad | [V] |
| Their own invariants | "A bus is atomic", "Never place a via on a BGA pad", "Emit only straight or 45-degree corners", "Throw on invalid input … do not silently return a partially routed bus" (`AGENTS.md`) | [V] |
| No published BGA score on the older repo | `tscircuit/autorouting` lists `hyperdense-*` ("Super dense BGA routing") as **TBA**, with no sample count; the repo says it is largely replaced by `tscircuit-autorouter` | [V] |

### Geometry of the two benchmarks (why they are hard)

Computed from the fixture constants. [C]

- **srj19/srj20 (0.8 mm pitch, 0.36 mm pads, 0.15 mm track, 0.10 mm clearance, 0.30 mm via).** The gap between two
  balls is 0.44 mm: one track fits (0.15 + 2·0.10 = 0.35 mm); two do not. A through via fits in the interstice between
  four balls (0.16 mm from the pad corners), but a via in a channel needs 0.30 + 2·0.10 = 0.50 mm > 0.44 mm, so
  **a via in an interstice closes both channels it touches on both layers**. With two layers, every escape is "a
  track in a channel" or "a via at an interstice", and the two compete for the same sites.
- **fanout31 AM62L (0.5 mm pitch, 0.254 mm pads, 0.08128 mm track, 0.05 mm trace-to-pad).** Gap 0.246 mm: one track
  (0.181 mm), not two. An interstice via (0.24 mm, edge-to-pad 0.08128 mm) fits with 0.05 mm to spare; a row of such
  vias is a wall for a crossing track. tscircuit's README says the same for a BGA400 on four layers: "cannot
  honestly route the full BGA400 on four layers with the retained 0.10 mm rules" because adjacent 0.15 mm via
  centres are 0.40 mm apart and a crossing 0.10 mm trace needs 0.45 mm [V].

## B. Literature on escape routing (positive results that matter for the plan)

| Work | What it gives | Tag |
|---|---|---|
| T. Yan and M. D. F. Wong, "A correct network flow model for escape routing", DAC 2009, pp. 332–335 | Earlier flow models ignored **diagonal capacity** (45° routing), so they either produced illegal routes or missed a legal routing. Corrected model gives the first optimal algorithm for (unordered) escape routing, with missing pins | [A] abstract read on hkbu scholars |
| L. Luo and M. D. F. Wong, "Ordered escape routing based on Boolean satisfiability", ASP-DAC 2008, pp. 244–249 | Existing ordered-escape approaches "cannot guarantee to find a routing solution even if one exists". An exact SAT formulation; good results on industrial escape problems | [A] abstract |
| Ozdal and Wong, simultaneous escape routing and layer assignment (ICCAD 2004; TCAD 25(8), 2006) | Models multi-component escape as *longest path with forbidden pairs*; exact polynomial algorithm for the **maximal planar subset on one layer**, then layer-by-layer assignment of the rest; randomized for large instances | [A] |
| Yan, Kong, Wong (and Ma, Young, Wong, DAC 2011) on bus layer assignment | The common heuristic assigns a maximum subset of buses to each new layer in turn; a branch-and-bound algorithm gives the **minimum number of layers** on industrial data | [A] |
| Gao, Dong, Tang, Yu, MCMCF-Router (TODAES 29(6), 2024); MC-MCF (ISQED 2023); MORE-Router+ (ISQED 2024) | Multi-capacity ordered escape as multi-commodity flow; grid arrays up to 308 pins, 100 % routability; multilayer ILP on a 50×50 array, 54 buses, 1,602 nets, ≤ 266 s with "net sequence division" (4–156× faster) | [A] |
| Liu, Lin, Chen, He, Chen, Chang, DAC 2023 | Matching-based hierarchical sequencing with variable design rules; 100 % routability, no violations on all given industrial PCB instances | [A] |
| Lin et al., "A complete PCB routing methodology with concurrent hierarchical routing", DAC 2021 (NYCU) | SER stage completes layer assignment of all nets and gives an escape order, then post-SER refinement and **gridless** area routing | [A] |
| B-Escape (ISPD 2010; cited in doc 05 for trial ordering) | Simultaneous escape by boundary routing with dynamic net ordering | [A] title and one slide-deck summary only; authors not checked |
| Sattar and Ignjatovic, "Simultaneous escape routing using network flow optimization", MJCS 29(2), 2016; Ali et al., PeerJ CS 2021 | Flow/ILP models that solve net order and escape together | [A] |

## C. Catalogue of approaches that have been shown not to work (or not to work alone)

Each entry says what was tried, the evidence and its strength, and what TraceMaker should take from it.

### C1. From the literature

| # | Approach | Why it fails | Evidence | Consequence for TraceMaker |
|---|---|---|---|---|
| L1 | Network-flow escape with unit or uniform capacities and no **diagonal capacity** | Produces routes that violate diagonal spacing, or reports infeasible when a legal routing exists (Yan and Wong 2009) | [A] | Any flow model must cap each gap by the narrower diagonal, as `route/escape_flow.cpp` already does (doc 05 §14) |
| L2 | Unordered escape (flow) used where the destinations are **ordered** | Flow chooses *which* balls leave through which channel, not *where each ends up*; matching pin order to a fixed destination order is a different, harder problem (Ozdal and Wong; Luo and Wong) | [A] | The srj19/srj20 data shows this (doc 16 §3): `escape --flow` plans every ball on the pad layer for boards neither router can finish |
| L3 | Route one component freely, then match the other to its order | Leaves nets unconnected (as summarised from Sattar and Ignjatovic) | [A] second-hand | Plan both ends together or plan the crossing structure first |
| L4 | Heuristic ordered escape (maze, A* + rip-up) as the only method | "Cannot guarantee a solution even if one exists" (Luo and Wong); a rip-up router can also cycle | [A] | Keep an exact or complete method for the last window (doc 16 F4) |
| L5 | Greedy layer assignment (take a maximum subset per layer, one layer at a time) | Not minimal in layers; optimal assignment by branch and bound needs fewer | [A] | Layer assignment should be a joint, bus-aware step (F2), not "route what fits, push the rest down" |
| L6 | Treating the 2-layer BGA problem as pure fan-out | Wire congestion on layer 1 can exceed the limit when vias are assigned without regard to routing (2009 IEICE 2-layer BGA via assignment, MILP) | [A] | Via-site choice and track routing must be decided together (F3) |
| L7 | Exact SAT/ILP on whole boards | Complete but does not scale; MORE-Router+ needed "net sequence division" for a 50×50 array. No SAT-vs-ILP runtime study was found | [A]; scaling claim from the papers' own framing | Use exact solvers on windows only (≤ ~40 nets) |
| L8 | The Rectangle Escape Problem and bus layer assignment | NP-complete; an LP-relaxation approximation exists | [A] | No polynomial algorithm to hope for; windows and heuristics are the design |
| L9 | BGA-pattern escape reused for general PCB fan-out | The pin array in a BGA is regular, general PCB pins are not (AAAI paper that proposes deep RL for fan-out) | [A] | Keep array-specific planning for array packages only; the general router must still work on irregular pads |
| L10 | Reinforcement learning on dense layouts | RL that picks routing costs "struggles with high-density designs" (Khan and Rovinski, 2026, chip detailed routing, fix: history-aware offline RL); PCBWorld (arXiv 2607.05915) reports an RL policy trained on synthetic boards only "approached" rule-based routers zero-shot, and says rule-based routers rarely complete complex production boards end to end | [A] | Do not make learned ordering a dependency of this work |
| L11 | Dog-bone fan-out at fine pitch under ordinary fab rules | Altium: at 0.5 mm pitch dog-bone is not possible under standard capabilities, so via-in-pad is needed; vendors disagree on the cut-off (0.65 mm vs 0.8 mm for via-in-pad) | [A] vendor material, not a standard | Via-in-pad must be an explicit, rule-gated capability; tscircuit's own sets forbid it |
| L12 | Vendor "AI escape router" claims (e.g. a 1,200-pin FPGA in hours) | Marketing, no independent numbers | [A] | Not evidence |

### C2. From tscircuit's own repositories (merged and closed PRs, README notes)

| # | Attempt | Result | Evidence |
|---|---|---|---|
| T1 | Four-layer BGA400, 0.4 mm corner-interstitial case | Declared unroutable: a via row is a copper wall (0.40 mm via-centre spacing vs 0.45 mm needed). Kept as a regression test | [V] `fanout-solver` README |
| T2 | RP2350A QFN60 breakout with decoupling caps outside the boundary (`fanout-solver` #23) | Routed 0/27; reverted (#26): "there genuinely was no space" | [V] |
| T3 | Widening the shared boundary around the same RP2350A (#28) | **More room routed fewer connections**: 13/23 at the tight box, 0/23 at +0.3 mm … +3 mm. Filed as a solver defect (monotonicity invariant) | [V] |
| T4 | "Fix AM62L fanout timeouts and route constrained top-facing layouts" (#141) | Reverted by an open PR (#147) | [V] titles only |
| T5 | Optimising to the input's 0.05 mm trace-to-pad minimum (`tscircuit-autorouter` #2389) | The unchanged benchmark evaluator uses **0.10 mm**, so routes at the stated minimum still failed it; DRC errors went up (31→36 and 32→45) before the fix used the evaluator's distance | [V] |
| T6 | Pipeline 10 BGA pair fan-out, vertical/axis-aware variants (#2340, #2347), topology-merging experiments (#1863, #2575) | Closed without merge; #2575 left a 460.8 s case above the 360 s limit and unresolved CI failures | [V] titles/bodies |
| T7 | Greedy plane-site choice | One plane's greedy choice can consume another's last site; fixed by a joint selection capped at 10,000 states with eight alternatives per blocked plane | [V] `docs/core-progressive-dram-performance.md` |
| T8 | Fixed route order for buses | A wide bus blocked by its peers; needed bounded retries that promote the blocked bus ahead of its peers | [V] |

### C3. From TraceMaker's own measurements (doc 05, doc 10)

| # | Attempt | Result |
|---|---|---|
| M1 | Flow-based escape (`--escape-flow`, doc 05 §14) | Routes fewer connections than the simple corridors (7 boards: flow 2,703, corridors 2,736, no plan 2,687): reserving long channels takes room the perimeter pins need |
| M2 | Escape corridors and flow on srj19/srj20 (doc 10 §7, 10 M, one variant, 50 boards) | None helps: srj19 66 % → 60 % (plan) / 60 % (flow); srj20 34 % → 32 % / 28 % |
| M3 | Routing dense-package connections first, then shortest-first | logicbone 964 → 750, decelerator 479 → 431 |
| M4 | Group re-route of up to five connections in every order (old R4) | Same routed count on all nine nearly clean boards and 24 boards (doc 05 §17) |
| M5 | Global router corridors (square tiles, 8 pitches) | Cost completion; the congestion map is neutral (doc 05 §13, §16, §19) |
| M6 | Per-pin escape analysis as a feasibility test | Says "every pin escapes" on the srj19 boards both routers fail (e.g. samples 11, 19, 31, 117): **individual escape is not joint feasibility** |
| M7 | More search budget | srj19 stops improving between 50 M and 200 M expansions (88 % on the first 50 boards); srj20 still rises (46 % → 56 %) |
| M8 | Portfolio of eight variants at 10 M | About the same as one variant at 50 M (88 % / 48 % on the first 50) |

### C4. From the M14 experiments (2026-10-11, doc 16 §10)

| # | Attempt | Result |
|---|---|---|
| X1 | tscircuit's routes as soft/hard path guides | 440–499 of 570 connections routed vs 499 unguided: topology realised in another router's geometry does not complete a board |
| X2 | Max-cut layer assignment of connection chords (two layers) | srj20 gap 18 % → 14 % clean |
| X3 | Deep-pins-first / longest-first order | 68.6 % / 54.9 % routed vs 76.7 % |
| X4 | Negotiate from the first pass; limit victims per route; growing crossing cost; history weight | 72.7 %; 2–7 % clean vs 28 %; 16 % vs 23 %; no effect |
| X5 | More seeds (24 variants) on one hard board | all end at 33–40 of 52 |
| X6 | Finer lattice (20 µm) at equal work | 61.6 % vs 76.7 % |

## D. Open questions the sources do not answer

1. Whether the 17 srj19 and 43 srj20 boards neither router solves are infeasible. Nobody has published a feasibility
   bound for them; tscircuit's failures are mostly timeouts.
2. How tscircuit's `highDensityForceImproveSolver` works internally (the web write-up named in its README was not
   found); its role is inferred from its name and the timing above only.
3. SAT versus ILP runtime on escape instances of 50–150 nets: not found.
4. Whether `dataset-fanout31` plane-drop cases are satisfiable on fewer layers than the fixtures use.

## Sources

- tscircuit: [tscircuit-autorouter](https://github.com/tscircuit/tscircuit-autorouter),
  [fanout-solver](https://github.com/tscircuit/fanout-solver),
  [dataset-fanout31-am62l](https://github.com/tscircuit/dataset-fanout31-am62l),
  [dataset-srj19](https://github.com/tscircuit/dataset-srj19), [dataset-srj20](https://github.com/tscircuit/dataset-srj20),
  [autorouter-benchmark-dashboard](https://github.com/tscircuit/autorouter-benchmark-dashboard) (run JSON
  `data/runs/38105514608-1.json`), [autorouting](https://github.com/tscircuit/autorouting),
  [bga-fanout-solver](https://github.com/tscircuit/bga-fanout-solver), [dogbone-solver](https://github.com/tscircuit/dogbone-solver),
  PRs fanout-solver [#28](https://github.com/tscircuit/fanout-solver/pull/28), [#26](https://github.com/tscircuit/fanout-solver/pull/26),
  [#147](https://github.com/tscircuit/fanout-solver/pull/147), [#158](https://github.com/tscircuit/fanout-solver/pull/158);
  tscircuit-autorouter [#2389](https://github.com/tscircuit/tscircuit-autorouter/pull/2389),
  [#2575](https://github.com/tscircuit/tscircuit-autorouter/pull/2575), [#1863](https://github.com/tscircuit/tscircuit-autorouter/pull/1863).
- [Luo and Wong, ASP-DAC 2008](https://www.cecs.uci.edu/~papers/aspdac08/pdf/p244_3A-4.pdf) (PDF not readable by the fetch tool; abstract via
  [UIUC experts](https://experts.illinois.edu/en/publications/ordered-escape-routing-based-on-boolean-satisfiability/)).
- [Yan and Wong, "A correct network flow model for escape routing"](https://scholars.hkbu.edu.hk/en/publications/a-correct-network-flow-model-for-escape-routing/).
- [Algorithms for simultaneous escape routing and layer assignment of dense PCBs](https://scholars.hkbu.edu.hk/en/publications/algorithms-for-simultaneous-escape-routing-and-layer-assignment-o/),
  [Optimal layer assignment for escape routing of buses](https://scholars.hkbu.edu.hk/en/publications/optimal-layer-assignment-for-escape-routing-of-buses/),
  [An optimal algorithm for layer assignment of bus escape routing on PCBs](https://scholars.hkbu.edu.hk/en/publications/an-optimal-algorithm-for-layer-assignment-of-bus-escape-routing-o/).
- [MORE-Router+ (ISQED 2024)](https://isqed.org/English/Archives/2024/Technical_Sessions/124.html),
  [MC-MCF (ISQED 2023)](https://isqed.org/English/Archives/2023/Technical_Sessions/103.html).
- [Simultaneous Escape Routing using Network Flow Optimization](https://ejournal.um.edu.my/index.php/MJCS/article/view/7006),
  [Mobility based net ordering for SER](https://saiconference.com/Downloads/Volume8No7/Paper_62-Mobility_based_Net_Ordering_for_Simultaneous_Escape_Routing.pdf),
  [PeerJ CS 499](https://peerj.com/articles/cs-499) (403 to the fetch tool; abstract only).
- [PCBWorld, arXiv 2607.05915](https://arxiv.org/abs/2607.05915); [RL for dense layouts (Khan and Rovinski)](https://tldr.takara.ai/p/2609.08232);
  [Altium: matching via types to BGA pitch](https://resources.altium.com/p/how-match-pcb-types-bga-pitch);
  [JLCPCB escape routing guide](https://jlcpcb.com/blog/effective-escape-routing-strategies);
  [Quilter FPGA case study](https://www.quilter.ai/blog/case-study-how-quilter-ai-routed-a-1200-pin-fpga-bga-in-hours-a-deep-dive-into-dense-bga-automation) (marketing).
