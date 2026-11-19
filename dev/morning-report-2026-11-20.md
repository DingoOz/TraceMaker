# Morning report, 2026-11-20

Night of 2026-11-19, about 11:00 to 19:30 UTC. Asked: commit the rule fit, open a PR, start the next step, then work about eight hours
without stopping for questions and record the assumptions.

## Summary

- **19 of the 33 failing PCBench boards now have a complete output**, against 3 before. By the best arm per board open connections go
  890 → 245, of which 129 are one board (d20). KiCad 10.0.6 finds 0 unconnected items on every newly complete board (on at least one of its complete outputs).
- **`route --finish`** is the command that does it: one run per board (`--finish --finish-masks --finish-layers 2,4`, 20 M) took 29 of
  the 30 open boards from 881 open to 219 and completed eight. Aleste-520EX completes in 40 minutes. 127 of the 219 are d20.
- The joint re-route (the step I set out to build) **does not beat more budget** and is not a default step. It is useful inside the ladder.
- **At 15:05 UTC the machine ran low on memory and the session stopped all my background runs.** I had too many jobs going, and one board
  (mini_mass at six layers) uses over 100 GB on its own. Some measurements are unfinished because of it (below).
- Nothing is pushed beyond PR #93. The new work is local commits on branch `joint-reroute` in `TraceMaker-rm3`.

## What was done

1. **PR #93** (https://github.com/DingoOz/TraceMaker/pull/93): rule fit and chain measurement (D194). It also carries the 17 M24-M26
   commits that were only on `m26-impl`.
2. **Repair along the route (D195).** `--repair-select route|both`, `--repair-focus`, `--repair-variants`. After a 20 M run on the 33
   boards: 28 of 887 open connections closed by the end windows, 30 by the route selection (39 % less work), 47 by both; no board
   completes. Three times the budget closes 159 and completes three more. Gate not met.
3. **`route --finish` (D196).** Rungs on top of one another: the run as given; fitted rules with focused repair; pad mask openings
   (`--finish-masks`); more layers (`--finish-layers 2,4`). Only what is open is routed at each rung, so a rung cannot end worse.
4. **Pad mask openings from a reference (D197).** Nine designer layouts run tracks through other nets' pad mask openings (KiCad reports
   them on the designer's boards too). `--fit-rules-from` now relaxes that when the reference does it; `--soft-pad-masks` by hand.
   memsarray completes for the first time.
5. Docs: doc 20 §2.11-§2.13 and three progress rows, D195-D197, roadmap M27, doc 18 G4, G7 and a new G8, `dev/progress.json`, devlog,
   CLI reference. Record: `bench/records/finish-2026-11/` (README with every table).

## Results (33 boards, open connections; boards complete)

| | open | complete |
|---|--:|--:|
| Default rules, 20 M (base) | 890 | 3 |
| Default rules, 60 M (control) | 731 | 6 |
| Rules fitted from nothing, 20 M (yesterday) | 771 | 6 |
| Base, then fitted rules on top | 597 | 7 |
| … then 2 more layers | 296 | 8 |
| … then 4 more layers (32 boards) | 274 | 10 |
| Best arm per board, no designer layout | 245 | 16 |
| Best arm per board, designer layout allowed as reference | 233 | 17 |
| … plus the stronger ladder on the boards 1-2 short (60 M, from-nothing rung, 8 repair variants) | | 19 |

First-time completes tonight: Aleste-520EX (250 open on every earlier run → 0 at six layers), P8000 (+2 layers), MicroMaple (+2 or +4
layers), memsarray (pad masks), ardfpga (fitted rules on top of the 60 M run), LeeChee (repair with eight variants, or 60 M), serial_gw and
teensy-fx (60 M alone), DronPi, Teensy_test_layout and dorkyboard (the stronger ladder, at their own layer count).

`route --finish --finish-masks --finish-layers 2,4` as one command, 29 boards: 881 → 219 open (739 → 92 without d20), eight complete, six
more 1 short. With `--finish-fresh` PCIE completes too (30 → 2 → 0).

## Checks

- Quick tier: 30 of 30 outputs byte-identical with the new options off (`finq` against `fitq-new`).
- `ctest --preset release`: passes; `delay_room` and `fixture_preparation_native` skipped as before. A first run was void because I rebuilt
  the test binary while it ran; the rerun is the one that counts. One change came after it (the ladder writes its output after every kept rung); the
  finish, fit and repair tests pass with it.
- Not run: the asan preset, `mkdocs build --strict` (mkdocs is not installed here).

## Stopped and unfinished

At 15:05 UTC the session stopped six background commands for low memory, among them the dashboard on port 8775 (started in an earlier
session; **it is still down, I did not restart it**). The notice said not to start them again unasked, so I did not. Unfinished:

- the ladder on top of the 60 M run: 14 of 33 boards done;
- the seed spread on the boards one or two short: 2 of 8 boards done (the seed changed nothing on those two);
- the focused repair on d20;
- `topmask` on 4 boards, `toplay4` on mini_mass.

The scripts in `bench/records/finish-2026-11/scripts/` skip what is done. Run them fewer at a time, and leave mini_mass out of the layer
rungs or cap its memory.

Instead I ran `route --finish` end to end, three jobs at a time with a memory guard. mini_mass grew past 100 GB there too (fourth rung, six
layers, in the relax rescue) and I stopped that one process by hand.

## Assumptions

A95-A105 in `dev/assumptions.md`. The ones you may want to overrule:

- **A96:** the new work is not pushed and has no PR. It sits on `joint-reroute`, on top of `rule-fit` (PR #93).
- **A100:** extra layers and soft pad masks are opt-in rungs, never automatic.
- **A102:** the repair options stay off by default.
- **A103/A104:** the stopped runs were not restarted; a smaller run was started in their place.
- **A105:** "complete" is the router's count; KiCad was run on the newly complete outputs only.

## What is left on these boards

- **d20, 127 open** after `route --finish` (142 at base). Layers change nothing on top: 27 pins are walled in by tracks of the earlier
  rungs.
- **Still short after `route --finish` at 20 M:** robomezzi 1 (also on the stronger ladder), TCKB 3, OtterPill 3, motor 3, E3 4, V2X 4,
  poncho 6, LFK78 8, CoreOne 10, sizif 10, MonApollo 12, EtherCAT 16; mini_mass 5 when it was stopped. The stronger ladder was only run on
  the eight boards one or two short; these may gain from it too.
- **OtterPill** completes only when the rules are fitted from the designer's layout, from nothing.
- **Memory.** mini_mass at six layers: one variant on one thread reaches a 30 GB cap in 34 s, before it routes anything
  (`std::bad_alloc`). So it is the size of the search state for that board and layer count (no leak found), times eight variants. The
  router has no check of memory before it starts; a user's machine would be exhausted the same way.

## Suggested next steps

1. Decide whether `joint-reroute` goes up as a PR on top of #93.
2. Finish the stopped runs with a memory cap; the ladder on the 60 M run looked like the strongest arm (of the 8 boards it reached that
   the 60 M run had left open, 5 completed: fifogfx, test-jig, ardfpga, memsarray, MicroMaple).
3. G8 in doc 18: pins walled in by earlier rungs (d20), and the boards 1 short. The from-nothing rung is built (`--finish-fresh`).
4. The memory growth on mini_mass.
