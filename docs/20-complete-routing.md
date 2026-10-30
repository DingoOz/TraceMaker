---
status: plan
code: [src/route/legalize.cpp, src/route/cut_test.cpp, src/app/route_job.cpp]
---

# 20 — Towards 100 % routed boards (plan)

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Written 2026-10-30 after D159 (the cut test, pads as they are in the
> legaliser) and D160 (the relax rescue by default). The steps are worked in the order of §3. Each one gets its results and a line in
> §4 when it is done.

## 1. Where things stand

**The question** (2026-10-30): how do we get an algorithm that always routes a routable board completely? And would routing with
impossibly thin tracks first, then thickening them, help?

**What was learned that day** (doc 05 §67, §68; `bench/records/d159-thin-cut`):

- **The cut condition is a cheap, reliable certificate.** The test of Leiserson and Maley (STOC 1985) and of Cole and Siegel checks every
  pair of nearby pads or vias: do the tracks crossing between them fit with their widths and clearances? `tracemaker cuts` checks it in
  about 0.3 s per board. On 405 KiCad-clean boards (423,870 cuts) it raised no false alarm. It is a necessary condition only:
  track-to-track spacing away from pads, zones and the edge is not checked.
- **On ball grids, thin-then-thicken fails only at vias.** On the six stuck srj20 boards, solved at 0.06 mm, every track fits between pads
  at 0.10 mm. Four vias provably have no room: a cell's diagonal is 0.622 mm, and a via plus a track need 0.72 mm.
- **On real boards, relax lost through a modelling bug.** The legaliser saw rotated pads as bounding boxes and ovals and rounded pads as
  sharp rectangles. With pads as they are, the first hand-over keeps every route on d20 and poncho. On the 33 failing PCBench tier B-D
  boards, relax went from −483 to −241 against the plain run and completes 5 boards against 2.
- **Relax's remaining loss is the final hand-over** (80 % of the clearance to the board's rules). On poncho the cut test finds room
  (clearance scale s\* 1.167), yet the projection leaves 71 pairs short by 0.02-0.04 mm. More sweeps (3000) and a larger move box do not
  help: the projection is stuck, not slow.
- **The default run now escalates** (D160). A board left open is routed again with relax, on its own budget, and the better result
  is kept. Default runs end on their time limit on hard boards (the plain portfolio is still working at 120 s), so the rescue gets
  `--time` again.

**What "100 %" can mean.** Some boards cannot be completed as placed. For those, the goal is a proof and a fix the user can act on
(§2.4), not more search. For the rest, completion is a search problem: the srj20 plateau was the lattice's wasted room, not capacity
(doc 05 §41).

## 2. The candidates

### 2.1 A full `--relax-late` run (cheapest)

`--relax --relax-late`: plain rounds under the board's rules first, relax only when they stall, one budget, best legal result kept. A
run stopped after 16 of the 33 boards had +46 against relax (ErgoDone +27, poncho +19). It also left fewer boards clean (2 against 4),
was 5 behind the plain run, and ran with an engine whose in-pad restore was too strict (D159). A full run with the current engine settles
whether it beats the rescue on some boards.

- **Cost:** one 33-board run (about 2 h at 3 jobs).
- **Measure:** against the plain run and the rescue (D160), per board.
- **Stop rule:** drop it if it does not beat the rescue on routed count or clean boards.

### 2.2 A local rescue (warm start)

The rescue routes the board again from nothing. It could instead start relax from the plain run's routes, which are legal under the
real rules and so also under relaxed ones. Only the open connections, and what they rip, would then be routed at 60 % of the clearance.
That should be faster, and should keep what the plain run got right on boards where a fresh relax run loses elsewhere.

- **How:** the plain run writes its state (`--state-out`). The rescue reads it (`--state-in`, D102) and starts its relax portfolio from
  it.
- **Cost:** hours (the warm start and state files exist).
- **Measure:** a third arm on the 33 boards, warm start against fresh start, both kept only if better than the plain run.
- **Stop rule:** keep the warm start only if it wins on the sum and on clean boards, or matches them in less time.

### 2.3 A realizer for the final hand-over (largest expected gain)

Where the cut test says a topology fits and projection stalls, solve for all the vertex positions together. Two methods:
- **LP or QP with a trust region:** linearise every clearance constraint at the current positions, solve for a move within a box, and
  repeat. The LP prototype in the srj20 investigation reached legal positions where projection left pairs (sample039).
- **Shortest homotopic paths with spacing,** the "rubber band" sketch (Leiserson and Maley; Gao et al.): pull each route tight in its
  homotopy class with spacing, then place vias in the room the cut test found.

- **Cost:** days. Start with the LP: it slots in after the projection, only when pairs are left.

- **Measure:** poncho's 71 pairs; then the 33 boards under `--relax`; then the rescue.
- **Stop rule:** it must keep every result exact-checked and deterministic (fixed iteration counts, stable ordering).

### 2.4 Tell the user why, with a proof

`tracemaker cuts` and the stuck report's `boxed_in` already know when a pin cannot get out or a gap cannot hold its tracks. Turn them
into a message that names the fix, for example: "U3 pin 7 cannot escape: the gap between pads 6 and 8 holds 0.32 mm and needs 0.40 mm.
Move U3 by 0.1 mm or narrow the track to 0.10 mm."

- **How:** run the cut test on the final routing and on the open connections' straight lines. Report over-full pad-to-pad cuts with
  the shortfall and the cheapest change: a move, a width or a clearance.
- **Cost:** a day for the report and its viewer events; longer for suggested moves that are checked.
- **Measure:** on the 33 boards, which open connections get a proof, and whether the suggested change makes them routable.

### 2.5 More rungs on the default ladder

Run each rung only while connections are open, and keep a result only if it is better:
- **Pin and gate swaps** (`bench/pin_swap.py`).
- **Small placement nudges** (`--place eco` or `refine`).
- **More copper layers** (`--try-layers`: the layer advisor picked the right count on 28 of 28 boards, D138).

Rule relaxation (the real-world closure loop got 7 of 8 boards to zero open) changes the design's rules. It stays a step the user
approves, not a silent default.

- **Cost:** a rung each. Swaps and nudges change the design, so they are offered as a result to accept, like the layer advisor.
- **Measure:** completion and clean boards on the 33, and what each rung changed.

### 2.6 Via room as a cost on ball grids (research)

On srj20 the thin sketch fails only where a cell has no room for its via. While routing at thin clearance, count each cell's via room
(the cut test's via room) and charge for using the last of it: thin geometry, thick accounting. Blanket real-clearance vias
(`--ramp-vias-real`) cost stage 1 too much and gained nothing (D159).

- **Cost:** research. Measure on srj20's six stuck boards first.

## 3. Order

1. **§2.1 relax-late:** cheap, and it answers a question left open.
2. **§2.2 local rescue:** cheap, and it improves the default directly.
3. **§2.3 realizer:** the largest expected gain.
4. **§2.4 proofs for the user:** helps every user, also when 100 % is impossible.
5. **§2.5 more rungs.**
6. **§2.6 via room:** research.

Each step:
- runs the 33 failing PCBench tier B-D boards (`bench/sets/pcbench-bd-fail.txt`, 100 M work, eight variants, KiCad-judged) against the
  current default;
- runs the quick tier when it changes the default;
- keeps its records in `bench/records/<step>/`, writes a decision record, and adds a line to §4.

## 4. Progress

| Step | Date | Result | Decision |
|---|---|---|---|
| Cut test, exact pads | 2026-10-30 | relax −483 → −241 against plain; 5 clean against 2 | D159 |
| Relax rescue by default | 2026-10-30 | 16,208 routed (+42 against plain), 8 better and 0 worse, 5 clean against 2; CPU +73 % | D160 |
