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
