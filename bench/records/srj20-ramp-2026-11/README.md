# srj20 never-clean boards: ramp campaigns, 2026-11-15 to 2026-11-17

The tscircuit srj20 set has 200 boards, each with one 0.8 mm pitch BGA (13×13 or 11×11, 0.36 mm pads on F.Cu) and
edge test points. Before these runs, 185 boards had been routed clean at least once; 15 never had. These runs took the
ledger to **189/200 ever clean**. The 11 that are left are hard because of the board itself, not the budget (see "Why
they stay open").

Binary: main 7333069. Rules are the board's own: two layers, track 0.12 mm, clearance 0.1 mm, via 0.3/0.15 mm. Every
result was judged by KiCad DRC (`drive.judge`: no unconnected items, no added errors).

## Recipe (all arms)

```
--relax --variants 16 --threads 8 --fast-bends --no-kb --round-work 20000000 --rounds 1000 --time 3600 --time-total 3600
--ramp-from 0.06 --ramp-step 0.02 --ramp-rounds 3 --ramp-cycle 2 --ramp-stall 2   + --seed N --warm-pcb <start>
```

The ramp starts at 0.06 mm clearance and steps up to the real rules. A board is clean when one arm reaches 0 open.

## What made each board clean

| Board | Start (warm board) | Arm / seed | Time | What changed |
|---|---|---|---|---|
| sample128 | best October result, 1 open (stack-srj20-s4-g0) | a1, seed 11 | 561 s | the ramp from a near-complete board |
| sample103 | complete routing at 0.06 mm (diag-c006) | a5, seed 15 | 340 s | loose-clearance start |
| sample072 | complete routing at 0.06 mm (diag-c006) | a6, seed 16 | 1481 s | loose-clearance start |
| sample096 | srj20-never a3 result, 1 open | a6, seed 16 | 1090 s | same recipe, different seed, warm from this week's best |

The command lines are in `clean/`. The routed boards are not committed: srj20 has no licence (local use only, doc 16 §1). They stay in `bench/results/srj20-never/sample{128,103,072}` and `bench/results/srj20-loose/sample096/a6`.

## Experiments

### 1. srj20-never (`never/`): six arms per board, warm from the best earlier results and from the diag-c006 loose routings

3 clean (128, 103, 072). Open at the end: 152 3, 040 2, 096 1, 070 3, 064 8, 176 9, 120 10, 008 15, 088 17, 144 15,
200 16, 032 18.

### 2. srj20-loose (`loose/`): route at 0.06 mm clearance, then ramp to the real rules

Stage 1 (0.06 mm in the .kicad_pro, `--variants 16 --round-work 60000000 --rounds 200`, 1800 s), open at 0.06 mm:
096 0, 152 1, 040 3, 070 4, 064 9, 176 7, 120 5, 144 14, 008 14, 200 9, 088 17, 032 22.

Stage 2 ramps (a1-a4 from the loose board, a5-a6 from the best board so far):

| Board | Before | After | Best arm |
|---|---|---|---|
| 096 | 1 | **0** | a6, seed 16, warm from srj20-never a3 |
| 152 | 3 | 2 | a5 (best so far) |
| 120 | 10 | 5 | a3 (loose start) |
| 176 | 9 | 9 | — |
| 200 | 16 | 14 | a1 (loose start) |
| 008 | 15 | 14 | a5 |
| 144 | 15 | 14 | a6 |

040, 070, 064, 088 and 032 were skipped (their loose routing was no better than their best).

### 3. srj20-again (`again/again.py`, `rounds.jsonl`): six option arms × 4 threads, 1 h, warm from the best board

Arms: a1 `--lns-open 8`; a2 the same plus `--lns-open-radius-mm 5`; a3 `--island-slack 2`; a4 island + lns; a5
`--ramp-from 0.04`; a6 `--ramp-fresh 1`.

| Round | Board | Before | After | Notes |
|---|---|---|---|---|
| r1 (plain repeat) | 040, 152 | 2, 2 | 2, 2 | every arm ended at its start |
| r1 options | 120 | 5 | 5 | a4 6, a5 7 |
| r1 options | 070 | 3 | **2** | a5 `--ramp-from 0.04` |
| r2 options | 070, 040 | 2, 2 | 2, 2 | 040 a5 went to 8 |

### 4. First nets (`again/first.py`, `first.jsonl`): route the connections that stay open in most saved runs first

"Hard nets" are the connections open in at least half of a board's saved results. Arms: a1/a2 warm from the best + `--first-nets`;
a3/a4 from nothing + first-nets; a5 from nothing (control); a6 from nothing + `--first-nets-ordered`. 4 threads, 1 h.

| Board | Hard nets | Before | Warm a1/a2 | From nothing a3/a4/a6 | Control a5 |
|---|---|---|---|---|---|
| 040 | 080, 070 | 2 | 2 / 2 (080 now routed, 070 still open) | 12 / 15 / 12 | 8 |
| 120 | (5 nets) | 5 | 5 / 5 | 25 / 22 / 23 | 21 |
| 152 | 054, 061, 055 | 2 | 2 / 2 (055, 061 still open) | 8 / 17 / 14, none of the hard nets open | 7 |
| 070 | 036, 047 | 2 | 2 / 2 (both still open) | 13 / 8 / 9, none of the hard nets open | 11 |

From nothing, first-nets routes the hard nets every time, but leaves as many or more others open.

### 5. Layer direction (`again/layer.py`, `layer.jsonl`)

One board at a time, three arms from nothing, 16 threads each, 3300 s, seeds 401-403. a1 `--layer-dir F.Cu=h,B.Cu=v
--layer-dir-pen 1.0` (fixed directions, strong penalty); a2 `--layer-dir-pen 2.0` (the router's own direction choice, which is on
by default on breakout boards at 0.3, with a much stronger penalty); a3 no options (control).

| Board | Best before | a1 fixed h/v, 1.0 | a2 auto, 2.0 | a3 control |
|---|---|---|---|---|
| 040 | 2 | 9 | 9 | **6** |
| 152 | 2 | 6 | 5 | **4** |
| 070 | 2 | 6 | 7 | **5** |

A stronger direction penalty is worse on every board. A human would use one direction per layer, but forcing that with this
penalty costs routes: the router needs the freedom to jog. A side result: the 16-thread controls from nothing (4-6 open in
55 min) beat the 4-thread fresh arms of experiment 4 (7-11 open in 1 h) on the same boards, so threads (variants run in
parallel) matter more than the hour.

## Why they stay open (`images/`)

The pictures of every open connection are in the published page (draw.py renders them; build.py builds the page).

- **No room for the via.** 95 of the 105 open balls have none of their 4 dog-bone via sites free: other nets' tracks or vias sit
  on them (`sites.py`, `sites.json`).
- **Too many vias, in the wrong places.** The best boards have 300-360 vias (3-4 per connection). Only 1-9 of the 60-80
  vias inside the BGA are on dog-bone sites.
- **Boxed in from outside.** 38 of the 105 open balls are on the outer ring, closed off by vias placed just outside
  the package.
- **Long, crossing connections.** The open connections run 9-21 mm across the board. Counting straight-line crossings
  between connections (`crossings.py`, `crossings.json`) predicts the open set: the 11 boards rank 1-9, 11 and 16 of 200
  (median 1242 crossings, against 171 for the boards that were always clean and 847 for the 4 that only finished this
  week).

What a human would do: fan the BGA out first with a reserved dog-bone via per ball; keep a band about 1 mm wide round the
package free of vias; one direction per layer with 1-2 vias per connection; route the long cross-board connections first.
sample032 (18 open, the most crossings) may need a third layer.

## Router finding

`router.cpp` (around line 8030) keeps `best_final` only from states reached at the real rules after the ramp
(`if (best_final_routed > pr.best.routed) pr.best = best_final`). The warm-start board is never counted, so an arm can
return a worse board than it was given (rule 4: commit only if the score improves). The drivers here always kept the best
board across arms, so no result was lost, but the router should start `best_final` from the warm board when that board is
legal at the real rules. Not fixed.

## What made a difference and what did not

- Warm starts that are already legal at the real rules are a fixed point: re-ramping them almost never changes the open
  count. Two exceptions: 096 (new seed, 1 → 0) and 070 (`--ramp-from 0.04`, 3 → 2).
- A loose-clearance start helps only when it has far fewer open than the best board (120 10 → 5, 200 16 → 14, and the
  diag-c006 starts that made 103 and 072 clean).
- The same connections fail run after run. Routing them first gets them routed, but other connections fail instead.
- `--lns-open`, `--island-slack`, `--ramp-fresh 1` made no difference here; a stronger `--layer-dir-pen` made it worse.
- Fresh 4-thread 1 h arms are far worse than warm arms (7-25 open against 2-5).

## Status after these runs

189/200 ever clean. Still open (best): 152 2, 040 2, 070 2, 120 5, 064 8, 176 9, 008 14, 144 14, 200 14, 088 17, 032 18.
