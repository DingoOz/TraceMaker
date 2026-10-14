# M16 notes: advice files and a pluggable advisor (issue #26), 2026-10-14

Design: [docs/17-llm-advisor.md](../docs/17-llm-advisor.md). Decisions D109 to D112. Data and test outputs: [bench/records/m16/](../bench/records/m16/README.md).

## What was asked

"See a recommended fix for the approach in PR 17 (agent skills); the fix is a new feature in issue #26 (LLM advisor: advice files and a pluggable
advisor, called only when the router is stuck); implement." Then: "make sure test results are captured in the repo" (hence `bench/records/m16/`,
this file, and doc 17 section 8: `bench/results/` is git-ignored, so results used to live only in docs and notes).

## What was built

| Stage of the issue | Built | Where |
|---|---|---|
| 0 stuck report | structured `RouteResult::Unrouted` (reason code, rips, fails, pad positions, blocker nets), `failure` event `region` and `blocker_nets`, the JSON report with disclosure and redaction, escape analysis, cuts, escape rings | `src/route/router.{hpp,cpp}`, `src/advice/stuck_report.*` |
| 1 advice file | strict parser and board check, hints (nets first, priority, layer cost and no-tracks, net-scoped regions as rule areas + disallow rules), `--advice`, advised portfolio variants, `advice-check` | `src/advice/advice.*`, `route_portfolio`, `src/app/` |
| 2 prove the channel without a model | `bench/advice_eval.py` (plain, advisor, equal-compute control), `advisors/heuristic_advisor.py`, `bench/advice_table.py` | gate NOT met, doc 17 section 8.1 |
| 3 advisor protocol and reference advisor | `--advisor`, `--advisor-timeout/-rounds/-redact`, `--advice-out`, `--stuck-report`, process runner, `advisors/tracemaker_advisor.py` (OpenAI-compatible and Anthropic) | `src/advice/advisor.*`, `advisors/` |
| 4 placement | not built | doc 17 section 9 |

Tests added: `tests/test_advice.cpp` (8 cases, 187 assertions), `tests/integration/advice_cli.py`, `advisors/test_advisors.py` (12 tests).

## Things that went wrong or nearly did (kept because they will recur)

1. **A work-budget experiment is only deterministic if the wall limit never fires.** The first three experiments did not pass `--time`; the
   default 120 s ended variants early on a loaded machine; the same plain command gave 54, 50 and 54 connections on one board. Advice looked
   like +3 to +11 connections. Found because the "plain" columns of experiments that ran the same command disagreed. Invalid runs (kept out of
   the records): plain totals 428, 425 and 432 for the same 8 boards where the valid runs give 430 every time. `advice_eval.py` passes
   `--time 3600` now.
2. **A control that gives the arms the same compute.** Without the 12-variant plain control, the advised arms (8 variants + 4 advised) would
   have looked like a gain over the 8-variant plain run on some boards. The control showed the extra variants alone do as much, and on the
   held-out half 11 connections more.
3. **The first advised-variant design repeated work.** Advised copies of variants 0, 2, 4, 6 are identical to those variants when the hints do
   nothing, so they cannot tell "no effect" from "harmful". The final design advises the list's next variants (the control's own), which
   makes advised-versus-control a pure test of the hints. The held-out half was looked at with the first design before this change, so its
   result for the final design is not an independent held-out measurement; the two designs agree (439 and 439), and the control is the same
   number in both.
4. **KiCad reads `a && b || c` as `a && (b || c)`** (doc 05 section 34), so regions generate only `&&` and `!=`/`==`.
5. **An ignored signal stays ignored across exec.** The advisor runner ignores SIGPIPE while it writes to a pipe; the child restores the
   default before exec (a test reads the child's `SigIgn` mask).
6. **The shared `llama-server` on this machine crashed** (HTTP 503, "engine stopped unexpectedly (exit code -6)") on the first real-model
   smoke test and timed out on the second (150 s). No further attempts: it is a shared service on nearly full GPUs. The model path is
   verified against fake servers only.
7. **`ctest` in a worktree skips fixture tests** (`bench/data` is git-ignored): link it in (`ln -s .../bench/data bench/data`) or the skip
   list hides the tests that matter.

## Open

- A real model has not written advice for this router (see 6). `advisors/tracemaker_advisor.py --dry-run` prints the prompt.
- Stage 4 (placement seams) and per-net layer preference and guide corridors are not built; section 8.1 says the simple hints do not
  help, which is why.
- The hint the evidence favours, "run the relax-route-legalise profile" (doc 05 section 41.3), needs its own design (doc 17 section 9).
- Extra seeded variants helped the held-out half by 11 connections at 20 M work (doc 17 section 8.1). Whether a 12-variant default for
  breakout boards is worth its cost was not studied here.
