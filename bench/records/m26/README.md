# M26: finish the BGA boards that never complete (doc 18 §21, D191)

## F8: the warm start is the ramp's floor (doc 05 §84)

srj20 sample040, warm from its best board (2 open, `srj20-again/sample040/first/a1`), a ramp that runs out of rounds while relaxed:

```
--relax --variants 4 --threads 4 --fast-bends --no-kb --round-work 1000000 --rounds 2 --time 300 --time-total 300
--ramp-from 0.01 --ramp-step 0.02 --ramp-rounds 1 --seed 5 --warm-pcb <best>
```

| Binary | Open | KiCad judge |
|---|---|---|
| main 7333069 (`f8/old`) | 16 | — |
| F8 (`f8/new`) | 2 | 2 unconnected, no added error |

The scoring round routes the same connections and moves some copper (370 of 1,243 segments, 111 of 356 vias differ from the warm board). The unit test
(`clearance ramp from a warm start`) starts a ramp from a complete 24-connection board; without the floor it returned 17 routed. ctest: 476
tests, all passed (2 skipped as before). The quick tier was not run: the default route cannot reach the change (the relax rescue turns the floor
off, and nothing else starts a ramp from a warm start).

## F1: where the via sites are lost

**No srj20 run ever reserved a site.** The breakout portfolio (D101), which every srj20 run uses, never turns on the escape plan
(`escape_corridors` is 0 in every best result). Doc 16 §3.4 had measured that reserving corridors lowers the clean rate from nothing.

**What takes the sites** (`f1/blockers.py`, the eleven boards' best results): of the 420 dog-bone sites of the 105 open balls, 12 are free.
A site can have several blockers:

| Blocker | Sites |
|---|---|
| a track on F.Cu (all of them BGA nets, passing between the balls) | 364 (87 %) |
| a track on B.Cu | 356 (85 %) |
| a via inside the BGA, not on its own ball's site | 109 (26 %) |
| a via outside the BGA | 26 (6 %) |

None of the blocking vias is another ball's own dog-bone via. The sites are lost to tracks that thread the interstices on both layers, which
is the competition doc 16 §3.6 describes: a via in an interstice closes the channels it touches.

## Probes on the three boards with 2 open (152, 040, 070), warm from the best board

Each arm used the srj20-never recipe with the same seed (501) and ran for 1800 s. The binary has F8, so no arm can end below its start.
Arms run 5 at a time per board.

| Probe | Arms (`probes/escape.py`) | 152 / 040 / 070 open |
|---|---|---|
| escape plan (16 variants, 5 threads) | control, `--escape-plan`, `--escape-flow` | 2 / 2 / 2 in every arm |
| (8 variants, 3 threads) | control, `--rescue 0.5,1,2`, `--rescue 1,2,4 --rescue-victims 32`, `--pin-via-pen 2`, `--via-cost-mm 6` | 2 / 2 / 2 in every arm |
| end game (8 variants, 3 threads) | `--endgame 0.3` alone and with three rescue settings (up to `--rescue 2,4,8 --rescue-victims 64`) | 2 / 2 / 2 in every arm |

- With a warm start, the escape plan reserves a corridor for every pin (75, 87 and 91 corridors: all of them), because it is planned
  before the warm routes are taken over. Nothing is aimed at the open balls.
- The pin rescue runs only inside the end-game retry (`--endgame`), so the first rescue arms did nothing. With the end game on, it tried 3
  rescues per run and routed none.
- Even 0.04 mm below the board's clearance the ramp held 89 of 91 on 040, with the same two connections open, and their reason is "boxed in;
  negotiated: transaction rolled back". The router can cross other nets, but it cannot re-route the nets it would displace. The congestion is
  board-wide, so a local repair of the warm board does not reach it.

## F2: reserved sites from nothing

The same three boards, from nothing. Each arm ran for 3300 s with 16 variants on 6 threads, seed 403 (`probes/fresh.jsonl`):

| Board | Control | `--escape-plan` (dog-bone sites + perimeter corridors) | `--escape-flow` (sites and channels by flow) |
|---|---|---|---|
| 152 | **7** | 8 | 9 |
| 070 | **7** | 9 | 12 |
| 040 | **6** | 9 | 14 |

Reservations lose on every board, as doc 16 §3.4 found on the whole set in October. Blocking other nets out of a ball's site costs more routes
than it saves. F2 as specified (hard reservations) is dropped, and so is F3 as a hard band. What is left is guidance: a via in the wrong
place costs more, but nothing is forbidden.

## F2, F3 and F5 as guidance, from nothing

The same boards, budget and seed as the F2 table (`probes/guide.jsonl`). Nothing is reserved; the arms change costs only. pv is
`--pin-via-pen 2`: another net's via in a pin's 1 mm fan-out region costs double (F2 as guidance). band is the same with the region widened
to 1.5 mm, which covers a band round the package (F3 as guidance). vc is `--via-cost-mm 6`: every via costs twice the default (F5).

| Board | Control | pv | band | vc |
|---|---|---|---|---|
| 152 | **7** | 8 | **7** | **7** |
| 070 | **7** | 9 | 8 | 9 |
| 040 | **6** | 9 | 12 | 14 |

No arm beats the control on any board. A dearer via costs routes on 040 (14 against 6) and does not lower the via count much (292-312 against
294-326). The cost guidance tried here does not reproduce what a designer does: the router's congestion comes from tracks threading the
interstices (F1), and pushing vias away moves the congestion without removing it.

## F4: move a blocking via

On the eleven boards' best results, none of the 420 sites of the open balls is blocked by vias alone: every one also has another net's
track on it (`f1/blockers.py`, the "via only" count). Moving vias alone frees no site. `--via-shove` already moves a via together with the
tracks that end at it (D156), but it runs only in the strict main loop. In the relax and ramp runs no connection reaches it (`shove.jsonl`:
2 / 2 / 2, no shove tried).

So the shove was run without the relax stage: a plain route warm from the same best boards with `--via-shove --via-shove-mm 1.5`,
2 variants, 40 M work (`probes/shove2/`). It tried 133, 114 and 180 via moves on 152, 040 and 070 and kept none. All three stayed at 2
open. Moving the vias near a boxed-in ball, with their tracks, does not open a path: the tracks that block the site also run on past
it. F4 is not built further.

## F7: long, crossing connections first

F7 adds `--order 4` (most straight-line crossings with other connections first, then longest first) and `--order-all N` (one connection
order in every variant of the portfolio). Before this, the breakout portfolio drew each variant's order from shortest first, deep pins first
or jittered, and `--order` reached variant 0 only. The test is the F2 setup, from nothing (`probes/order.jsonl`):

| Board | Control | `--order-all 4` (crossings first) | `--order-all 1` (longest first) |
|---|---|---|---|
| 152 | **7** | 10 | 12 |
| 070 | **7** | 8 | **7** |
| 040 | **6** | 13 | 12 |

Ordering on its own loses, as first-nets did in the srj20 campaign: the long connections get through and short ones that had room fail
instead. The portfolio's mixed orders are worth more than any single order. The options stay, off.

## F9: crossings in the summary and a layer hint

Every route now writes `line_crossings` to the summary: pairs of connections whose straight pad-to-pad lines cross (lines that share a pad
and connections to a zone do not count). On a two-layer board with open connections, when there are at least 11.5 crossings per connection,
the "next" advice says two-layer boards this crossed were not routed clean in testing and suggests `--try-layers 2,4`.

Calibration on all 200 srj20 boards with the router's own count (`f9/calib.py`, `f9/calib.json`): the eight boards at 11.5 or above (032 14.6,
008 13.6, 088 13.2, 200 12.9, 120 12.4, 176 12.4, 144 12.2, 064 11.8) were all never routed clean. The next is 128 (11.0, made clean this
week). The other three never-clean boards (070 10.9, 152 10.8, 040 10.6) are under the line, among boards that do route (127 10.7,
103 10.3). On sample032 the run prints: "the connections' straight lines cross 14.6 times each (1577 crossings for 108 connections)".

## F6: lanes under the package

`--package-lanes PEN` (doc 05 §86): for connections at least 8 mm long, a far-layer track cell inside the package that is off every ball
row and column line costs PEN pitches. The setup is the F2 one, from nothing (`probes/lanes.jsonl`):

| Board | Control | `--package-lanes 1` | `--package-lanes 2` | `--package-lanes 1 --package-lanes-mm 4` |
|---|---|---|---|---|
| 152 | **7** | 8 | 10 | **7** |
| 070 | **7** | 12 | 8 | 9 |
| 040 | **6** | 10 | 16 | 13 |

Lanes lose on every board but tie once (152 at 4 mm). Keeping the long connections to straight lanes costs the router the jogs it uses
to get past congestion, as the stronger layer-direction penalty did in the srj20 campaign (experiment 5). The option stays, off.

## Summary

| # | Built | Result | Adopted |
|---|---|---|---|
| F8 | warm start is the ramp's floor | sample040 lossy ramp 16 → 2 open | yes (on for warm ramps) |
| F1 | blocker analysis | tracks take 85-87 % of the open balls' sites; 12 of 420 free | (analysis) |
| F2 | existing `--escape-plan`, `--escape-flow`; `--pin-via-pen` | from nothing: 8-14 and 8-9 against 6-7 | no |
| F3 | `--pin-via-pen 2 --pin-via-mm 1.5` | 7-12 against 6-7 | no |
| F4 | existing `--via-shove` | 0 of 427 shoves kept; 2-open boards stay at 2 | no |
| F5 | `--via-cost-mm 6` | 7-14 against 6-7 | no |
| F6 | `--package-lanes` (new) | 7-16 against 6-7 | no (option, off) |
| F7 | `--order 4`, `--order-all` (new) | 7-13 against 6-7 | no (options, off) |
| F9 | `line_crossings` + two-layer hint (new) | flags 8 boards, all never-clean, no board ever routed clean | yes (report only) |

The milestone's gate (≥ 3 of the eleven clean, ≤ 50 open in all) is not met: no candidate beat the control on the three closest boards,
so the gate run on all eleven was not made. Each probe is one seed per arm. The control's own spread from seed to seed is about ±1-2
(experiment 5 of the srj20 record had 4-6 at 16 threads), and most arms lost by more than that.

## No change to the default route

The base is the main 7333069 build (the source this branch started from), and the new build has F6-F9 with the options off. `bench/run.py
--set quick --work 1000000 --route-args "--variants 1"`: 30 of 30 boards byte-identical. `--set mid --work 20000000`: 11 of 11
byte-identical. Both have the same summary lines (quick 29 open of 669 connections; mid 223 of 3,674). ctest: 477 of 477 pass.
