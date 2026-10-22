---
status: plan
code: []
---

# 18 — Candidate features for the roadmap (research and plan)

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Written 2026-10-14 after M16, issues #4–#8 and #24 were closed.
> **Update (2026-10-16): M17 was built and measured (docs/10 §11, D121, D122, `bench/records/m17`). Two statements below are corrected by it: PCBWorld's "679 real boards" (§2, §3, A1) are derived from PCBench, so A1 is
> not an independent second corpus; and the paper was read in full (§1). The rest of this note is as written on 2026-10-14.**
> **Status (2026-10-15): adopted as the roadmap's M17–M21 on the owner's answers in §7 (decision D118). Nothing here is built.** The
> candidates keep the numbers they have in §4; §5 is the order that the answers gave. This note changes no code. Evidence tags: **[R]** checked in this repository (file or doc named), **[W]** from a web source (vendor
> claims are marked as self-reported), **[A]** my estimate or assumption.

## 1. How this was made, and what it cannot tell you

Read: the roadmap, the open ("not built", "still open", "next") passages of docs 04–17, the non-goals of doc 01, the morning reports, the
release page, and the current repository for each gap named below. Searched the web (US-only search tool, standard mode) for KiCad 10's
release notes, tuning profiles in KiCad's source documentation, the KiCad IPC API, the commercial AI layout tools, pin swapping, and 2026
papers on learned PCB routing.

Limits, so the ranking is read with them in mind:

- **No user research.** Nothing here comes from asking people what they need. The ranking follows evidence in the repository and the field,
  and two guesses about the audience (§7).
- **Vendor material is self-reported.** Quilter, DeepPCB and Flux publish comparisons of themselves; I found no independent validation [W]
  and quote no vendor number.
- **PCBWorld's paper was not read in full** when this note was written (only the abstract page); it was read in full on 2026-10-16 for M17, and its D3 set is derived from PCBench (docs/10 §11.1).
- **Nothing was run for this note** except reading. No candidate has a measurement of its own gain yet; each carries the experiment that
  would give one.

## 2. Where TraceMaker stands (facts to plan from)

| Fact | Source |
|---|---|
| Clean pass (KiCad judge, 8 variants, 120 s) A 100 % (40/40), B 70.0 % (28/40), C 63.3 % (19/30), D 54.5 % (12/22); Freerouting 2.5: 100 / 50 / 46.7 / 36.4 %. No board has an error added by routing | [R] `dev/morning-report-2026-10-07.md` |
| M8 gate (from-scratch clean pass 90 % on held-out set S) is at 85 %; M9 gate (dense-package boards) is at 41 %; M7's SA-PCB comparison was never measured | [R] memory note `tracemaker-plan`, morning report |
| What fails is fan-out at a package; PCBench boards are judged under KiCad's default rules, which their own human layouts mostly fail (6 of 92 pass) | [R] doc 05 §16–§20, doc 04 §10 |
| BGA breakout boards: srj19 about 95 %, srj20 78 % (160/200 with up to 433 s) against tscircuit's 92 % / 79 % | [R] README, doc 16 §10 |
| PCBench has no stackups, so impedance classes (doc 15 §15) and the plane-split features (doc 05 §42, §44) cannot be measured on the quick tier; the #24 measurement used four plane boards, one seed | [R] doc 15, `bench/records/issue24/README.md` |
| The router has one width per net; per-layer widths are not built | [R] doc 15 §15 (line 1642) |
| No pin or gate swapping; non-goal of version 1, "later, opt-in, with back-annotation" | [R] doc 01 §4 |
| `--relax` completes srj20 boards no ordering or reservation helped; I found no measurement of it on PCBench tiers B–D (searched docs 05 and 10) | [R] doc 17 §9, doc 05 §41.3 |
| PCBWorld (679 real boards, its published baselines in doc 10 §3) has a fetch script entry but was never fetched or run: `bench/data` has no `pcbworld` | [R] `scripts/fetch_fixtures.sh`, `ls bench/data`, doc 10 |
| Release v0.9.0 has one asset, the promo video. There is no binary and no plugin archive to download; building needs GCC 15 (and CUDA 12.x for the GPU) | [R] `gh release view v0.9.0` |
| The KiCad plugin was driven in a running KiCad 10.0.6 GUI (Xvfb, one undo step); not checked: that KiCad builds the plugin's virtual environment and shows its button | [R] doc 08, morning report 2026-10-07 |
| A long-running `tracemakerd` is not built; each plugin run starts the engine afresh | [R] doc 08 |
| Viewer: control channel (pause, step, seek), minimap, WebGPU backend, TLS not built | [R] doc 09 |
| Set-up takes 135 s on LeeChee (obstacle queries); GPU DRC broad-phase and GPU global routing not built, measured not to pay | [R] doc 05 §19, D55, D56 |
| #24's three options are off by default; `--si-rules` is not the best measured combination, and the effect was measured with one seed on four boards, where run-to-run noise is as large as some differences | [R] `dev/morning-report-2026-10-15.md` |

## 3. What the field is doing

- **KiCad 10 (released 2026-03-20)** [W: kicad.org release post; KiCad source documentation]. Track tuning was rebuilt: time-domain
  constraints (delay, not only length), **tuning profiles** with per-layer track width and differential gap, per-layer propagation delay, via
  delay overrides, a target impedance computed from the stackup, assigned to net classes; a graphical custom-rule editor; **PCB design
  blocks** (reusable layout modules); **pin and gate swap** with forward and back annotation (unconstrained; a constrained system "may come
  in future versions"); jumpers (same net, no ratsnest); inner-layer footprint graphics and keep-outs; suggested fixes for DRC errors; importers
  for Allegro and PADS. A `.kicad_pro` already has a `tuning_profiles` block (empty in the KiCad demos in `bench/data`) and the rule severities
  `missing_tuning_profile` and `tuning_profile_track_geometries` [R]; the engine reads neither [R: no match in `src/`].
- **KiCad IPC API** [W]: in KiCad 9 and 10 it talks to a running GUI only; headless use, plotting and export arrive with KiCad 11; KiCad 11
  stops bundling Python, so plugins move to their own interpreter. Third-party reports of editing a board through `create_items` and
  `push_commit` match what the plugin does [R].
- **Commercial AI layout** [W, self-reported]: Quilter targets complex DDR / PCIe / RF boards and returns several candidate layouts with a
  scorecard; DeepPCB presents learned global routing; Flux's auto-layout runs after the engineer has routed differential pairs, impedance-
  controlled lines and high-speed interfaces by hand. Common ground in how they sell: critical interfaces are where trust is lost, and
  buyers ask whether they can see why the tool did what it did and audit constraints and violations quickly.
- **Research** [W: abstract only]: PCBWorld (July 2026; open environment on KiCad, 679 real boards, eight engine-checked metrics) reports
  that tool-using agents beat grid-action RL and open-loop LLMs, that an RL policy trained on synthetic boards approaches rule-based routers
  on real ones, and that learning-based methods are not yet competitive with rule-based routers. OmniRouting (August 2026) is a benchmark
  for LLMs and multimodal models on routing.
- **Pin swapping** [W: Altium, Siemens, Cadence material]: described as a way to cut crossovers and layers, needs an up-front definition of
  which pins may swap, and must be passed back to the schematic. I found **no independent measurement** of the gain, and one trade article
  warns that a swap that looks harmless can break FPGA timing.

What this says for TraceMaker: its differentiators (determinism, exact checks, transactions, KiCad as judge, the stuck report, the audit
reports) fit "auditable". The field's open ground is high-speed interfaces done *with proof*; KiCad 10 just made the data for that
(per-layer geometry and delay) a native project setting that TraceMaker does not yet read.

## 4. Candidates

Effort: **S** one session, **M** two to three, **L** four or more [A]. Every candidate keeps the project rules (determinism, exact check,
a reference path for every accelerated path, off by default until measured, results in `bench/records/`).

### Group A — Measure on the field's data, and be installable

| # | Candidate | Why (evidence) | Effort | Gate |
|---|---|---|---|---|
| A1 | **PCBWorld-Bench run.** Fetch the set, route all 679 boards in the standard configuration, classify failures, compare with its published baselines (read the paper first) | The newest public benchmark of real KiCad boards; never run [R]; PCBench's weakness (default-rule judge, no stackups) makes a second, independent set worth having | S | A report with clean pass and completion per board class beside the published baselines, failure classes counted, and the boards TraceMaker loses to a baseline listed. No "better than" claim until the baselines' own settings are matched |
| A2 | **Prebuilt releases.** CI builds a CPU-only Linux x86_64 tarball and the KiCad plugin archive (PCM format) on a tag and attaches both; macOS arm64 if the CI runner allows | Release v0.9.0 has no binary [R]; GCC 15 is a high bar for the people the plugin is for | S–M | On a machine with no compiler: download, install the plugin archive, route `plane_smd` in under five minutes; a CI job does the same on a clean container |
| A3 | **A modern-boards set** with stackups and real interfaces (USB, DDR, PCIe, RF), 10 to 30 open-hardware boards with licences that allow storing them or a fetch script that pins commits | The SI features (D114–D117, impedance classes) are unmeasurable on PCBench [R]; repository boards (StickHub, CM5 Minima, a Jetson carrier) are few | M | Set committed as a manifest; every SI option run on it with the quick-tier rules (options off byte-identical) |
| A4 | **Version-drift watch.** A scheduled job runs the DRC parity corpus and the round-trip test against the newest `kicad/kicad` container, and against the KiCad 11 nightly when one exists | KiCad changes rule semantics between point releases (D80 followed 10.0.3) and 11 changes the plugin runtime [W] | S | A failing parity run opens an issue with the first divergent board |
| A5 | **Settle #24 with data.** Five seeds, twelve plane boards; choose the defaults of `--si-rules` from the result (the best measured combination is not what `--si-rules` is today) | The effect was measured with one seed and noise matches some differences [R] | S | A table with spread per option; `--si-rules` equals the combination that wins, or the doc says why not |

### Group B — High-speed done with proof (builds on D114–D117 and KiCad 10)

| # | Candidate | Why | Effort | Gate |
|---|---|---|---|---|
| B1 | **Read KiCad 10 tuning profiles.** Parse `tuning_profiles` and the net-class assignment; give the router **per-layer track width and pair gap** (microstrip on the outer layers, stripline inside), use KiCad's via and per-layer delays in the skew and length rules instead of the engine's own table (`src/crules/impedance_rules.hpp`), and match KiCad's `tuning_profile_track_geometries` rule in the DRC | Closes a gap the docs already name (one width per net) [R] and a KiCad 10 setting the engine ignores [R]. Format must be taken from a populated KiCad 10 project: the demos' list is empty | M | Boards with profiles route with each layer's profile width; `kicad-cli` with `tuning_profile_track_geometries` enabled adds 0 errors; DRC parity corpus extended with profile boards; options off byte-identical |
| B2 | **Time-domain matching.** Length and skew groups in picoseconds with per-layer and via delays (B1's data), meander tuning to a target delay, a check against KiCad's own length / skew rules | KiCad 10 headline feature [W]; DDR byte lanes and pairs are where the commercial tools aim [W]; the DRC already reads `ps` as a unit [R: `rule_engine.cpp`] | M | A DDR-style generated fixture and two real boards within tolerance by KiCad's DRC, in delay, not only length |
| B3 | **Close the audit loop.** Run the return-path stitching pass (`--stitch-vias return`) inside `--si-rules`, re-audit, and report unstitched layer changes and remaining detours as warnings with the net and the place | The audit (§42) and the stitch pass (§39.3) exist but are two commands; the issue's "hooks left" were exactly this [R]. Needs a reference plane net on two layers | S–M | On the four plane boards (and A3), unstitched layer changes of critical nets fall, 0 added DRC errors, quick/mid/planes unchanged with options off |
| B4 | **Parallel-run limits** (a geometric 3W / parallel-length proxy, report and cost, no simulation) | Crosstalk is the next thing after splits and vias on JLCPCB's list in spirit; but **no evidence of demand** in the repository, and doc 01 excludes simulation | M | Do not start without A3's boards showing parallel runs that matter. Listed so the question is asked |

### Group C — Routing and placement capability

| # | Candidate | Why | Effort | Gate |
|---|---|---|---|---|
| C1 | **`--relax` and `--txn` on PCBench B–D failures.** Run the failing boards of tiers B, C, D with `--relax` (and transactions), count the boards that complete | `--relax` finished srj20 boards nothing else did; no measurement on the tiers found [R]. The cheapest possible capability gain | S | Boards gained and boards lost at equal work, clean by KiCad's DRC; if it gains, a portfolio variant, else a recorded negative |
| C2 | **Pin and gate swap: feasibility experiment first.** Fixtures where swaps are legal by construction (identical gates of a multi-unit part, resistor-network pins, a connector-to-MCU bus on interchangeable GPIO) and a "free assignment" oracle: measure routed count, vias and layers with and without the freedom. Build the pass only if the gain is large | Vendors claim fewer crossings and layers; **no independent number found** [W]. KiCad 10 now applies swaps with annotation [W]. Wrong swaps break designs, so the eligibility list must be explicit (a file or the library), never guessed | Experiment S, pass L | Experiment: a table of routed / vias / layers with and without the oracle on at least ten boards. Build only if a median gain is clear; the pass writes swaps as a proposal file and applies them only when asked |
| C3 | **Pours and a layer-count advisor.** Create a ground (or power) pour on a free inner or back layer when a board has none; try 2, 4 and 6 layers and report the least that completes | doc 05 §18 says creating pours is "a possible feature (not built); it would close a few boards, not most" [R]; a layer count is a direct cost | M | On PCBench boards with no zones, the count completed with a generated pour vs without, DRC clean; the advisor's answer matches a brute-force run |
| C4 | **Repeated-channel replication.** Detect identical sub-circuits (graph isomorphism on the netlist, same footprints), route one, replicate its copper by transform, route the rest | Audio mixers, 3-phase drives, LED and ADC channels are common [A]; KiCad's multichannel tools and design blocks do it by hand [W]; `multichannel_mixer` is a fixture [R] | L | A research spike first: how many fixture boards contain a repeated block at all. Build only if several do |
| C5 | **Fan-out at packages (the failure class).** After C1, classify the dense-package failures of tiers B–D by cause (pin boxed in, via sites, layer count) and fix the largest class | The M9 gate is unmet at 41 %, and doc 05 says what fails is fan-out [R]. Without the classification the fix is a guess | Classification S, fix L | A table of failures by cause; the fix is chosen from it |
| C6 | **Pair tuning together** (both halves meandering, length-match of a pair to a bus), pairs ending on routed copper | doc 05 §15 lists both as not built [R]; the "crossovers for swapped pairs" item of the untracked `FEATURES.md` is partly done (pair twists, §15) | M | Pair fixtures: coupled share and skew within the limit with the pair tuned as one |
| C7 | **Large-board set-up time** (135 s on LeeChee): profile the obstacle queries, build the index once | Slow set-up is a blocker for the dense 8–12 layer boards N4 names [R] | M | Same output bytes, set-up time down by a stated factor on the largest five boards |

### Group D — Interaction, agents, explanation

| # | Candidate | Why | Effort | Gate |
|---|---|---|---|---|
| D1 | **Assistive use inside KiCad.** Route the selected nets or the area under a rectangle from the plugin, with a warm engine (`tracemakerd`) that keeps the board and caches | Doc 01 says users finish by hand in pcbnew; the plugin starts the engine afresh each run [R]; KiCad 11's headless IPC [W] would let it run unattended | M–L | A single-net route from the plugin within a stated latency on `multichannel_mixer`; still one undo step |
| D2 | **Engine-grounded agent API** (a tool interface, such as an MCP server, over the transactions: route a connection, rip a net, check, score, undo; every commit still passes the exact check) | PCBWorld's result is that agents with engine feedback beat open-loop and grid RL [W]; M16 found passive hints gain nothing [R], so the active version is the open question | M for the interface, value unknown | **Stop rule agreed in advance:** an agent loop (the rule-based advisor as the baseline agent) must route more than the plain run at the same compute on a held-out half; if not, the interface is kept as a library and nothing more is built |
| D3 | **A run report for designers.** One HTML / markdown file: what was routed, what is open and why (stuck report), which rules were read and which were not, the SI audit, the DRC comparison, the options used and the seed | "See why and audit quickly" is how buyers judge these tools [W, self-reported]; the pieces exist as separate outputs [R] | S–M | A report from `route --report FILE` for the four plane boards, every number traceable to a JSON field; reviewed by someone who did not write it |
| D4 | **Viewer control channel**: pause, step, seek on a recording, click a net to route it first, minimap | Not built [R]; value is explanation and debugging more than routing quality | M | Seeking in a recording within one second on the largest log |
| D5 | **Leftovers from #37 / #38**: `--view-token` / `--view-insecure` in the plugin and Python module, TLS (or a documented tunnel recipe), `--si-rules` map ignoring the antipads of vias the route adds | Named limits in the issue comments [R] | S | The plugin and module accept the flags; the antipad limit has a test |

## 5. Order (after the answers of §7)

The audience is hobbyists first, with high-speed work limited to the Kookabus boards; KiCad 11 support is wanted; a proposal-only pin swap is
acceptable; the agent interface is wanted, with the stop rule of D2. That puts install and measurement first, then the session server that the
agent interface and the KiCad assistant share, then KiCad 11 (due about February 2027 [W]), then the hobbyist wins, and the high-speed work last
and only as far as the Kookabus boards need it.

| Milestone | Candidates | Why here |
|---|---|---|
| **M17 — Installable and measured** | A2 prebuilt releases and plugin archive; A1 PCBWorld-Bench run; C1 `--relax`/`--txn` on the PCBench B–D failures | Hobbyists cannot use what they cannot install; the other two answer, cheaply, how TraceMaker does on boards nobody tuned it on, and whether a known capability is a free gain |
| **M18 — Session server and agent interface** | D2 (the agent API) on top of a long-running board session (`tracemakerd`, the core of D1) | Asked for. One session process serves two users: an agent that routes connection by connection, and the KiCad plugin that routes the selected nets without restarting the engine. The stop rule of D2 decides whether anything beyond the interface is built |
| **M19 — KiCad 11 readiness** | A4 made a milestone: file format, DRC parity and plugin for 11 | KiCad 11 is expected about February 2027 [W]: headless IPC (`kicad-cli api-server`), plotting and export through the API, no bundled Python. No official file-format change list was found [W], so the first task is the watch job, the second is reading a release candidate when one exists |
| **M20 — Hobbyist wins** | C3 pours and a layer-count advisor; D3 run report; C2's experiment and a proposal-only pin swap; C5's classification | Two-layer and four-layer boards are the common case; the layer count is a direct cost; a report makes a run auditable |
| **M21 — High speed for the Kookabus boards** | A3 reduced to the Kookabus boards; B1 tuning profiles; B2 time-domain matching; B3 the audit loop; A5 | Only as far as those boards need. The files must be found first (§7) |

C6, C7, C4 and B4 wait for a board that needs them.

## 6. Considered and not recommended now

- **A learned or RL router.** PCBWorld's authors say learning-based methods are not yet competitive with rule-based routers [W], and M12 measured that learned ordering and congestion models would learn little here (doc 05 §25) [R].
- **GPU global routing / GPU DRC broad-phase.** Measured to not pay (D55, D56) [R].
- **Signal-integrity simulation.** A non-goal (doc 01). The useful, cheaper form is to export what an external field solver needs (stackup, geometry), not to build one.
- **A gridless tile-plane arm.** Its measured upper bound is a few connections on the boards tried (doc 05 §25) [R].
- **Rigid-flex.** No demand in the repository or the sources read [A].
- **A hosted service.** The determinism and "your board never leaves the machine" are strengths; nothing here argues for it [A].

## 7. Decisions (2026-10-15)

| Question | Answer | Effect |
|---|---|---|
| Who is it for? | Hobbyists mostly; some high-speed users, "just the Kookabus boards" | Install, assistance and the layer count come before signal integrity; A3 shrinks to the Kookabus boards and B1–B3 are scoped to them |
| Boards you may keep | The Kookabus boards | **Open:** I found no Kookabus KiCad files on this machine (`~/kookabus-render` holds renders and promo tooling, `~/promo_render` the promo videos). Where are the `.kicad_pcb` files, and may they go in the repository (or a fetch script)? |
| KiCad versions | Support KiCad 11 | M19; the version-drift watch (A4) starts first |
| Pin swap | A proposal-only pass is acceptable | C2: a proposal file for review; the pass never writes swaps into a board. The feasibility experiment still comes before the pass |
| Agent interface | Do it | M18 with the stop rule of D2: if an agent loop does not route more than the plain run at the same compute on a held-out half, the interface stays a library and nothing more is built |

## 8. What could make this plan wrong

- B1 leans on KiCad 10 adoption: if the Kookabus boards (and hobbyists) do not define tuning profiles, B1 matters little. A3 will show it, and M21 is last for that reason.
- PCBWorld's boards may be mostly simple, so A1 could look good and teach little; the failure classification is the useful output, not the percentage.
- C1 may find nothing: `--relax` may need the breakout structure to help. A recorded negative still removes a guess.
- Vendor comparisons are self-reported; the pull toward "AI layout" features may be marketing rather than need.
- All effort figures are guesses from past sessions.
