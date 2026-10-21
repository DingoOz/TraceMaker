---
status: partly-built
code: [src/advice, advisors]
---

# 17 — Advice files and a pluggable advisor, called only when the router is stuck

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Written 2026-10-14 for issue #26. **Milestone M16.**
> Status: stages 0–3 built; stage 4 (placement) is not. §8 holds what was measured, including what did not work.

## 1. Where a language model belongs

TraceMaker is the deterministic counterweight to an LLM's probabilistic approach (requirement N3, rule 2). A model still has a
place: the *judgement calls* a lattice search cannot make — which part to move when `--cut-report` proves the placement cannot
route, which nets matter, which layer a group should prefer. This document puts it there and nowhere else:

- **Not inside the engine.** The tree has no HTTP client, no TLS, no credentials; a non-repeatable call inside a run that must be
  byte-identical would break N3.
- **Not a separate orchestrator.** Users would learn two tools, and the engine would still need the report and hint interfaces,
  which are the real work.
- **A replaceable advisor, called only when the deterministic route is stuck, talking to the engine through a file.**

```
 route board.kicad_pcb -o out.kicad_pcb --advisor "python3 advisors/tracemaker_advisor.py ..."

   plain portfolio ──► all routed? ── yes ──► done (the advisor is never started)
         │ no
         ▼
   stuck report (JSON) ──► advisor process (stdin → stdout, time-limited) ──► advice (JSON)
                                                                                 │ strict parse + check against the board
                                                                                 ▼
   portfolio again: the unadvised variants + `variants` more, carrying the advice
                                                                                 │ the better result of the two runs is written,
                                                                                 ▼ the advice is saved beside it (out.advice.json)
```

Three parts, each with one job:

| Part | Job | Where |
|---|---|---|
| Engine | Deterministic and offline. Writes a **stuck report**; reads an **advice file** (`--advice FILE`), typed and strictly checked, that can only change order, cost, and which attempts are made | `src/advice/`, `src/route/router.cpp`, `src/app/route_job.cpp` |
| Advisor | Any executable: stuck report on stdin, advice on stdout. A person, a script, a local or cloud model | `advisors/` (reference ones, standard library only) |
| The switch | `--advisor CMD`: plain route first; only if connections are left, consult, then route again with the advice. Without the flag nothing changes, byte for byte | `run_route_job` |

## 2. What it may and may not do (the project's rules)

| Rule | How it holds |
|---|---|
| 1 Correctness | Advice never contains copper and never changes a rule's value. Every route still passes the exact check; regions are enforced by it (§4) |
| 2 Determinism | The model's answer is frozen in a file. `board + options + seed + advice file` gives byte-identical output at any thread count (tested). A live run is repeated by `--advice out.advice.json` |
| 4 Transactions / never worse | Advised attempts are *extra* portfolio variants after the unadvised ones (the next entries of the list, indices from 1000). The winner is the minimum of the portfolio's total order, a tie goes to the unadvised variant, and in `--advisor` mode the plain result stays unless the advised run's is better |
| 5 Budgets | Advice changes no budget. The advisor's wall time is not routing work (`--advisor-timeout`); each routing run gets the same `--work`/`--time` |
| 6 Rules | A missing, slow, failing or nonsensical advisor is a warning; the run goes on unadvised, exit code unchanged |
| Untrusted both ways | The report quotes net and part names from the board (a prompt-injection surface). Advice is typed data: no commands, no paths, unknown keys are errors. Sending a board to a cloud model is opt-in, the report says what it contains, and `--advisor-redact` sends aliases |

## 3. The stuck report (stage 0)

`route --stuck-report FILE` writes it for the run the advisor sees (or the final run, with no advisor); `--advisor` sends the same
JSON on stdin. It is built from the finished route by `advice::build_stuck_report` (`src/advice/stuck_report.cpp`). Fields:

| Field | Content |
|---|---|
| `tracemaker_stuck_report` | 1 (the format version) |
| `task` | one paragraph telling an advisor what is asked and that names are data |
| `disclosure` | `redacted`, `contains` (kinds of data sent), `does_not_contain` (the board file, copper geometry beyond these lists, paths, keys, the advisor command) |
| `board` | name, outline box (mm), copper layers, counts of parts, pads, nets |
| `rules` | clearance, track width and minimum, via size and drill, copper-to-edge clearance |
| `result` | routed / connections, tracks, vias, seconds, the portfolio's variants and their counts, `escape_rings` (with `--escape-report`), `cuts` (with `--cut-report`) |
| `unrouted[]` | per open connection (up to `max_unrouted`, 40; `unrouted_not_shown` counts the rest): `net`; `from` / `to` (`part`, `pad`, `x_mm`, `y_mm`, `layers`; `to` is `{"zone": true}` for a plane); `length_mm`; `reason`; `why` (the router's own words; omitted when redacted); `rips`, `fails`; `blockers` (nets whose routed copper the last rejected route would have had to rip); `region_mm` (the two pads ±1.5 mm); `crowding` (routed copper by net in the region, vias and foreign pads in it) |
| `unrouted_by_reason`, `_by_net`, `_by_part` | counts, the largest 16 |
| `parts[]` | the parts at the open ends: kind (`grid_array`, `multi_pin`, `small_ic`, `passive`), side, pad count, position, number of open ends; `library_id` and `value` unless redacted |
| `previous_advice` | the advice already tried and what it gave (later rounds) |
| `advice_schema` | the hint types this engine reads, so an advisor adapts to the engine it talks to |

**Reason codes** (`failure_reason_code`, `router.cpp`): `boxed_in` (fixed copper encloses a pin), `exact_check` (the lattice path was
rejected by the exact check), `rip_limit` (the only route would rip a protected net, too many connections, or one already ripped too
often), `ripped_not_rerouted`, `layer_limit`, `out_of_budget`, `rolled_back` (a transaction was undone), `not_attempted` (the budget
ended first), `no_legal_path`. They are the class of the cause, matched from the router's sentences, and they nest: a boxed-in pin is
`boxed_in` whatever later attempts said.

**The `failure` event** (doc 13) now has `region` (the two pads ±1.5 mm, nm; it was `[0,0,0,0]`) and a new `blocker_nets` (net ids of
the connections the rejected route would have ripped). `blockers` (object ids) stays empty: doc 06's closed-set boundary analysis is
not built, and the victims are the cheap, true part of it.

**Redaction.** With `--advisor-redact` every net becomes `N1…`, every part `P1…` (in order of first use), `value` / `library_id` /
`why` are dropped, and the board name is `board`. The advice that comes back is mapped to the real names (`advice::unalias`) before it
is checked, and the saved file has the real names, so `--advice` repeats the run.

## 4. The advice file (stage 1)

```json
{
  "version": 1,
  "note": "free text, logged, never acted on",
  "variants": 4,
  "first_nets": ["USB_DP", "USB_DM"],
  "priority": [["U1.A1", "J2.3"]],
  "layers": {"no_tracks": ["In2.Cu"], "cost": {"F.Cu": 1.5}},
  "regions": [{"name": "north channel", "rect": [10, 10, 20, 14], "layers": ["F.Cu"], "reserve_for": ["NET1"], "vias": false}]
}
```

| Hint | Meaning | Existing mechanism it reuses |
|---|---|---|
| `first_nets` | these nets are routed first, also after restarts, and other nets do not rip them | `RouterOptions::first_nets` (`--first-nets`) |
| `priority` | these connections (pad to pad, either order; both pads on one net) are routed before the rest | `RouterOptions::priority`, which the knowledge base feeds and the command line does not |
| `layers.no_tracks` | no new tracks there; vias still pass | `no_track_layers` (`--no-tracks-on`) |
| `layers.cost` | a track on that layer costs this factor more (1 to 1000) | `layer_cost_pm` (`--layer-cost`) |
| `regions` | a rectangle or polygon (mm, the board file's axes) in which only `reserve_for` nets may put tracks and vias, or `avoid_nets` may not; per layer, tracks and/or vias | a **rule area plus `disallow` rules** (doc 05 §34) |
| `variants` | advised variants to add (1 to 8; `--advice-variants` overrides) | `route_portfolio` |

**Regions are rules, not a new obstacle type.** A region becomes an in-memory rule area (`tmk:advice:N`, no keep-out flags: it only
names a place) and `disallow track via` rules whose condition is `A.intersectsArea('tmk:advice:N') && A.NetName != 'NET1' && …`
("every net but these") or `… && A.NetName == 'NET3'` (one rule per avoided net), one rule per layer when layers are named. §34's
machinery then does what it does for a user's `.kicad_dru`: the lattice search treats the rule like fixed copper, and the exact check
before every commit, escapes, pair legs and clean-up enforces it. Two details matter. KiCad reads `a && b || c` as `a && (b || c)`, so
**no `||` is ever generated**. And the rules and the area live only in the router's working copy (`RouterOptions::extra_zones` /
`extra_rules`, added in `Router::Impl` and `Router::run`): the caller's rules are not touched and the output board never contains them.
(The positional rules are judged outside the per-class obstacle caches, so net-scoped regions do not need the caches keyed by net.)

**Strict reading** (`parse_advice`, modelled on the rule overrides, doc 15 §6.3): an unknown key, a wrong type, a value out of range
(coordinates within ±10 m, cost 1–1000, `variants` 1–8), a duplicate, a region with no area or with both or neither of
`reserve_for` / `avoid_nets`, or more than 64 `first_nets`, 512 `priority`, 24 `regions`, a 64-point polygon is an error that names the
file and the entry (`regions[2].rect: has no area`); a syntax error gives line and column. `check_advice_refs` then checks names against
the board: unknown nets, layers and pads are errors with a "did you mean", priority pads must lie on one net, `no_tracks` may not cover
every copper layer, net names with a quote or backslash cannot be used in a region. Entries without effect (a region outside the outline,
a `first_nets` net with fewer than two pads) are warnings. `tracemaker advice-check BOARD ADVICE` runs both checks without routing.
(Errors name the file and the JSON path of the entry; the line is given for syntax errors only.)

**Advised variants** (`route_portfolio`). With advice, the portfolio is the variants that would run anyway, then `variants` more: the
next entries of the portfolio's list, the very variants a plain run with that many more would have added (same configurations, same
seeds), each carrying the hints. They are named `<variant> + advice` and have indices `1000 + k` (`route::kAdvisedIndex`). So a plain run
with `V + 4` variants is the exact control for an advised run with `V`: the only difference is the hints. (A first version advised
*copies* of variants 0, 2, 4, 6. A hint that does nothing then repeats work already done, while the control's extra variants bring new
seeds; on the held-out half the control beat it by 11 connections, §8.) Variant results do not depend on each other except through
portfolio rounds, so adding variants at the end leaves the unadvised ones as they were (tested), and the total order puts a tie on the
unadvised side. The knowledge base is never told about advised variants. `route_result_better` is the same order for comparing two
whole runs.

## 5. The advisor protocol (stage 3)

`--advisor CMD`: `CMD` is run with `/bin/sh -c`, in its own process group, with the stuck report on stdin; stdout must be exactly one
advice file (at most 1 MB), stderr is logged (first 2,000 characters). After `--advisor-timeout` (120 s) the group is killed. Only the
program name is logged and put in the summary (a command line may carry a token). The answer goes through `parse_advice`, `unalias` and
`check_advice_refs`; if anything fails the run says why (`warning: the advisor's advice was refused: …`) and goes on without advice.
The accepted advice is saved as `<out>.advice.json` (`--advice-out` changes it; with `--advisor-rounds N > 1` also
`<out>.advice.K.json`), always with the real names.

`--advisor-rounds N` repeats the loop while connections are open, each time with the best result so far and `previous_advice` in the
report; every round routes from scratch with that round's advice alone, so the saved file of the last round repeats it.

Each routing run gets the full `--work` and `--time`. The advised run contains the unadvised variants again, so an `--advisor` run costs
about twice a plain one; `--advice FILE` alone costs one run with the extra variants.

## 6. Reference advisors (`advisors/`)

- `heuristic_advisor.py` — no model. Strategies `stuck-first` (nets of the open connections go first), `corridor` (a strip along the
  straight line of each open connection is reserved for its net, on the layer its start pad is on), `mixed`. It proves the hint channel
  (§8) and is the smallest example of the protocol.
- `tracemaker_advisor.py` — a language model through an OpenAI-compatible endpoint (llama-server, Ollama, vLLM, clouds) or the
  Anthropic Messages API. Standard library only. The key comes from the environment (`TRACEMAKER_ADVISOR_API_KEY`, else
  `OPENAI_API_KEY` / `ANTHROPIC_API_KEY`), never from the command line. An endpoint that is not on this machine or its network is
  refused unless `--allow-remote` is given; what is sent is printed to stderr first. The reply is parsed leniently (a code fence is
  tolerated), checked against the advice format, and sent back to the model once with the problems if it fails.
- `test_advisors.py` — both, against fake servers (no network, no model).
- **A coding agent needs no advisor process.** The `tracemaker-route` skill (`skills/`, D84) has the agent read `--stuck-report`, write the
  advice file itself, check it with `tracemaker advice-check` and route again with `--advice`; it puts that after more search (§8.1: the
  control wins) and says that the hint channel's measured gain is none. Whether a model's own hints do better is the open question of §8.1,
  not something the skill claims.

Benchmarks always run from **recorded advice files** (`bench/advice_eval.py` stores them), so results stay reproducible.

## 7. Options

| Option | Meaning |
|---|---|
| `--advice FILE` | use this advice (advised variants run next to the unadvised ones) |
| `--advice-variants N` | advised variants to add (1–8; default the file's, else 4) |
| `--advisor CMD` | consult CMD only if connections are left; route again with its advice |
| `--advisor-timeout S` | its time limit (default 120 s) |
| `--advisor-rounds N` | consult again with the advised result while connections are open (default 1) |
| `--advisor-redact` | aliases for nets and parts in the report |
| `--advice-out FILE` | where the advice that was used is saved (default `<out>.advice.json`) |
| `--stuck-report FILE` | write the stuck report |
| `tracemaker advice-check BOARD ADVICE [--json F]` | check an advice file against a board |

The summary (`--json`) gets an `advice` block: the file's hints and warnings, `unadvised_best_routed` / `advised_best_routed` /
`winner_advised` of the first run, and per advisor round the program, seconds, outcome or error, routed before and after, and whether the
advised result was kept. Viewer events: `advice.hint` for each hint, and `stage` events `advisor` begin/end.

## 8. What was measured

Raw data, the advice files used and the test outputs are in [`bench/records/m16/`](../bench/records/m16/README.md); the story of the
runs is in [`dev/m16-notes.md`](../dev/m16-notes.md).

### 8.1 Does a hint channel help? (issue #26 stage 2)

The question is whether advice makes this router route more, before any model is connected. Board set: the 16 `srj20` boards of fewer
than 70 connections that were still open after the 70 M portfolio run of M15 (`bench/sets/srj20-m15fail4.txt`: 8 + 8, even and odd index
as the tuning half and the held-out half). Configuration: `--work 20000000 --variants 8 --threads 8 --no-kb --time 3600` (the breakout
portfolio, 4 rounds), `bench/advice_eval.py`, a rule-based advisor (`advisors/heuristic_advisor.py`). Counts are connections routed, summed
over the half (489 and 492 connections; no board completes in any arm).

| Arm | Even half (tuning) | Odd half (held out) |
|---|---:|---:|
| Plain, 8 variants (what advice must beat) | 430 | 439 |
| **Control: plain, 12 variants** (the advised run's compute without advice) | 431 | **450** |
| Advice `mixed` (stuck nets first + a reserved strip along each open connection), first design (advised copies of variants 0, 2, 4, 6) | 431 | 439 |
| Advice `mixed`, final design (advised variants are the next four of the list, the control's own) | 431 | 439 |
| Advice `stuck-first` only (first design) | 430 | not run |
| Advice `corridor` only (0.35 mm half width, vias allowed through; first design) | 431 | not run |
| Advice `corridor`, 0.5 mm, other nets' vias allowed (first design) | 431 | not run |
| Advice `stuck-first` + layer cost F.Cu x1.4 + 12 priority pairs (first design) | 430 | not run |

What this says:

- **The gate is not met.** The gate was "advised runs route more on a held-out half, and never fewer anywhere". "Never fewer" holds by
  construction (no board did worse in any arm). "More" does not: on the held-out half advice routes exactly what the plain run does (439
  and 439), on the tuning half it gains one connection on one board (`sample094`, 54 to 55), the same total as the control, which gets it on
  other boards. No strategy tried on the tuning half beat the control.
- **Seeds beat these hints.** The control, the same four extra variants without hints, gains 11 connections on the held-out half
  (`sample173` 49 to 53, `sample136` 58 to 61). The same variants with the hints gain nothing: on the held-out half no advised variant routed more than the best unadvised
  variant of its own run, and on five of the eight boards they routed fewer. The first design hid this, because its
  advised copies repeated work that was already done and so could not tell "the hints do nothing" from "the hints hurt".
- **Consistent with the repository's earlier evidence.** M14 experiment E1 (doc 16): tscircuit's own finished routes as guides did not
  help either; the literature note puts language-model routing agents far below classical routers.
- These are the hints a rule can derive from the report (who is open, where, how crowded). A model might write better ones: judgement about
  which part to move or which nets matter is what the issue expects it to add, and the placement seams (stage 4) are where that would show.
  This measurement does not say it cannot; it says the channel gives no free gain, and that **the stage 3 gate of the issue (proving the
  channel without a model first) is not met**. Stage 3 is built because it was asked for, it is opt-in, and it cannot make a result worse.

An invalid first batch is worth recording because it nearly produced a false result: the driver did not pass `--time`, the default 120 s
wall limit cut variants short on a loaded machine, and the same command gave different plain counts in three experiments (one board: 54,
50, 54). Advice looked like +3 to +11 connections there. `advice_eval.py` now passes `--time 3600`, and every table above comes from runs
whose plain column agrees across experiments. (Lesson for any work-budget experiment: check that the arms' shared columns match.)

### 8.2 Checks

| Check | Result |
|---|---|
| Options off: `quick` / `planes` (plain and `--soft-zones`) / `mid` against `origin/main` (`49fbd3c`), CPU-only builds, `bench/compare_runs.py --expect-identical` | **byte-identical**: quick 30/30, planes 8/8 and 8/8, mid 11/11 (`mid` prints `Gates: FAIL` only because PR #28's judge calls three capped KiCad reports "unknown"; the baseline against itself prints the same) |
| Full test suite | 291 of 291 pass, release and CPU-only; the new tests and 11 router-heavy existing ones also pass under ASan + UBSan |
| Same advice file at 1, 2 and 4 threads, and twice: identical bytes | generated board (CLI test `advice_cli`, every run); a real `srj20` board (`sample100`, 1, 2, 4 and 4 threads): one sha256 |
| Bad advice (unknown key, name, layer, syntax error, every layer without tracks): refused naming the file | unit tests and `advice_cli` |
| Advisor absent, failing, slow, rubbish, unknown key, unknown net: output board and exit code as without it, the reason in the summary | `advice_cli` (5 failure kinds and a timeout) |
| Advised result never fewer than unadvised | by construction; no board worse in the 8 advised experiments (64 board runs) |
| KiCad DRC adds no error on advised boards | 0 router-introduced errors on 10 advised boards checked (5 of each design; `drc-advised.txt` has the final design's) |
| A real model | **not verified.** The reference advisor was tested against fake OpenAI-compatible and Anthropic servers only. One live request to the shared `llama-server` on this machine ended in HTTP 503 (its engine stopped, exit -6), a second timed out at 150 s; no further attempts were made on a shared service |

## 9. Not built, and limits

- **Stage 4 (placement).** Advisor-proposed placements as `seeds` of `routability_loop` and proposed moves as ECO candidates
  (`src/place/routable.hpp`) are not built: they would be kept only if they route more, through seams that exist, and wait for §8's
  evidence that hints help at all.
- **Per-net layer preference and per-net guide corridors** are new router features the issue asked to add only if the simple hints
  matter; §8.1 says they do not, so they were not added.
- **A hint that picks an attempt** ("run the relax-route-legalise profile", doc 05 §41.3) is the hint type the evidence of this repository
  favours: boards that no ordering or reservation helped (`srj20` 039, 048, 071, 103) completed with `--relax`. It is not built: relax
  drives the whole portfolio (stages, rounds, legalisation) and is not a per-variant tweak, so it needs its own design.
- A model that answers with many small, plausible hints costs only the advised variants' time; a model that answers with a region
  that fences a net out of its own pads can make an advised variant route less, which is why advised variants never replace
  unadvised ones.
- The report quotes at most 40 open connections; very large boards need `--advisor-rounds` or a better summariser.
