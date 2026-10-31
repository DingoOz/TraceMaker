---
status: partly-built
code: [src/route/legalize.cpp, src/route/cut_test.cpp, src/app/route_job.cpp, src/analysis/escape_proof.cpp]
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

**Outcome (2026-10-30, D163, doc 05 §71).** The warm rescue routed 16,259 against the fresh rescue's 16,208 (+51, 14 better and 5
worse) at 22 % less CPU, but left 4 boards clean against 5. It failed the stop rule by one clean board. The two are better on different
boards: the best of both per board is 16,280 (+72) with 6 clean. The default is now both: warm first, then fresh while connections are
open, at about 20 % more CPU on boards left open.

### 2.3 A realizer for the final hand-over (largest expected gain)

Where the cut test says a topology fits and projection stalls, solve for all the vertex positions together. Two methods:
- **LP or QP with a trust region:** linearise every clearance constraint at the current positions, solve for a move within a box, and
  repeat. The LP prototype in the srj20 investigation reached legal positions where projection left pairs (sample039).
- **Shortest homotopic paths with spacing,** the "rubber band" sketch (Leiserson and Maley; Gao et al.): pull each route tight in its
  homotopy class with spacing, then place vias in the room the cut test found.

- **Cost:** days. Start with the LP: it slots in after the projection, only when pairs are left.
**Prototype (2026-10-30).** `TM_LEGALIZE_DUMP=DIR` writes the legaliser's problem as the sweeps leave it. The Python prototypes are
in `bench/records/complete-routing/realizer/`. On poncho's final hand-over (1,718 vertices, 50,362 constraints, 71 pairs left by the
sweeps):

| Method | Pairs left (box 0.25 mm) | Pairs left (box 0.6 mm) |
|---|--:|--:|
| projection sweeps (today) | 71 | |
| SLP: linear programs with a trust region (scipy HiGHS) | 7 | 2 |
| penalty, L-BFGS-B | 92 | 12 |
| augmented Lagrangian, 6 rounds | 92 | 12 |
| SQP, QP by dual coordinate ascent (no LP solver) | 7 | 2 |

- The 71 pairs were two clusters. A ladder of about 20 parallel tracks, each a few µm short, needs to shift together, which the
  sweeps cannot do; the SLP and SQP solve it. A track in a 1.4 mm channel between two pad columns needs a bend that a straight segment
  cannot make within the box.
- Smooth penalties settle into a compromise in the squeezed ladder. Hard linearised constraints are what make the difference.
- The SQP needs no library, so it is the one ported to C++ (`route/legalize.cpp`, `LegalizeOptions::realize_iters`; experiment switch
  `TM_LEGALIZE_REALIZE=N`).

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

**Built (2026-10-30, D161, doc 05 §69).** The board-scale cut lines of `--cut-report` find nothing on tiers B-D, so the proof is per
pin. The board's fixed copper, grown by the clearances, is flood-filled from the pin on every layer, with via sites checked, at the
board's minimum track and via. A pin walled in there cannot be connected as placed. `tracemaker route` checks the pins of its open
connections (summary `why_open`), and `tracemaker walled` checks a board before routing.
- No false alarm on the 405 KiCad-clean benchmark boards (41,810 pins).
- On the 33 boards, none of the 1,341 pins of the 727 open connections is walled in by the placement. 8 are walled in only for their
  class's width (a neck-down gets out), 14 only by E3's net-less copper pours (`--soft-copper-graphics` frees them), and the rest are
  congestion. These boards have their designers' routings, so a proof could not fire on them; what the check gives here is the E3
  fix and the knowledge that the rest is a search problem.
- Suggested moves (which part, how far) are not built.

### 2.5 More rungs on the default ladder

Run each rung only while connections are open, and keep a result only if it is better:
- **Pin and gate swaps** (`bench/pin_swap.py`).
- **Small placement nudges** (`--place eco` or `refine`).
- **More copper layers** (`--try-layers`: the layer advisor picked the right count on 28 of 28 boards, D138).

Rule relaxation (the real-world closure loop got 7 of 8 boards to zero open) changes the design's rules. It stays a step the user
approves, not a silent default.

- **Cost:** a rung each. Swaps and nudges change the design, so they are offered as a result to accept, like the layer advisor.
- **Measure:** completion and clean boards on the 33, and what each rung changed.

**Outcome (2026-10-30): offered, not run.** What each rung is worth on these boards was already measured:
- Pin swaps (doc 05 §56): 982 → 978 open on 20 failing boards, 5 better and 5 worse, no clean board gained.
- Different placements (the placement closure experiment, 2026-10-16) made none of 8 failing boards clean, where more budget did better.
- More layers (D138): the advisor picked the right count on 28 of 28 boards. It is a stackup change the user pays for.

The escape proof (§2.4) agrees: no open pin on the 33 boards is walled in by the placement, so swaps and nudges have nothing to unblock.
Instead the run ends with "next:" lines by cause (doc 05 §69): a part to move for proved pins, `--soft-zones` or
`--soft-copper-graphics` where those free a pin, the neck-down settings for class-only pins, and more search or `--try-layers` for
congestion.

### 2.6 Via room as a cost on ball grids (research)

On srj20 the thin sketch fails only where a cell has no room for its via. While routing at thin clearance, count each cell's via room
(the cut test's via room) and charge for using the last of it: thin geometry, thick accounting. Blanket real-clearance vias
(`--ramp-vias-real`) cost stage 1 too much and gained nothing (D159).

- **Cost:** research. Measure on srj20's six stuck boards first.

**First measurement (2026-10-31).** The simplest form of thin geometry, thick accounting: in the relaxed stages a new via keeps the
board's clearance to fixed copper (pads, the board's own copper), and the lowered one to routed tracks, which legalising can still
move (`--ramp-vias-fixed`, `RuleEngine` untouched: `route/obstacles.cpp` adds `DesignRules::via_fixed_clearance_extra` to via
checks against items with no owner). srj20's six stuck boards under `--relax` (realizer on): 425 against 411 routed (+14, 5 better,
0 worse), same CPU; sample104 63 → 70, sample072 70 → 74. `--ramp-vias-real` (vias real to everything) had cost the thin stage too
much (D159). On the 33 failing PCBench boards under `--relax` (100 M work in rounds of 20 M, no rescue) it loses: 15,937 against
15,940 routed (1 better, 10 worse), 3 clean against 5 (serial_gw, ardfpga and 58r-mxalps lose their last connections; Teensy_test_layout
becomes clean), CPU +2 % (`bench/records/complete-routing/runs/vfix-bd.md`). Why has not been measured; a guess is that
where pads are not what limits a via, full clearance to them only takes room the relaxed stage needs. `--ramp-vias-fixed` stays a
hidden option for ball grids and is not a default; a per-cell via-room cost (the idea above) is still open.

### 2.7 A finer lattice (asked 2026-10-30)

Is the lattice's granularity hurting completion? Coordinates are int64 nanometres, so the number type is no limit. The search lattice
is: (width + clearance) / 6, 25-100 µm (doc 05 §25). `route --pitch-scale X` (hidden) scales it on every board and variant. On the
11 failing tier B boards at 20 M work: 2,051 routed at the automatic pitch, 2,040 at 0.67 and 2,041 at 0.5 (2 better, 5 worse each),
at 34 % and 117 % more CPU (`bench/records/complete-routing/pitch`). A finer lattice spends the budget on more steps per path. The
automatic pitch stays; the room it wastes is recovered off-lattice (relax and the legaliser).

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
| §2.1 relax-late, full run | 2026-10-30 | 16,079 routed (−129 against the rescue), 4 better and 15 worse, 3 clean against 5. Dropped (stop rule) | |
| §2.2 warm rescue | 2026-10-30 | warm 16,259 (+51 against fresh, 14 better, 5 worse) but 4 clean against 5; best of both 16,280 (+72), 6 clean: both passes by default, CPU +20 % on boards left open | D163 |
| §2.3 realizer (SQP after the sweeps), on by default | 2026-10-30 | poncho 71 → 7 pairs; `--relax` on the 33 boards +15 (5 better, 0 worse), srj20 six +7 (4 / 0) | D162 |
| §2.4 why open: escape proof | 2026-10-30 | 0 false alarms on 41,810 pins of 405 clean boards; on the 33 boards 0 of 1,341 open pins walled in by the placement, 14 by E3's pours (fix named), 8 by class width | D161 |
| §2.5 more rungs | 2026-10-30 | not run by default: swaps and placement gave nothing on these boards; the run's "next:" lines offer them by cause | D161 |
| §2.6 vias keep fixed clearance in relaxed stages | 2026-10-31 | srj20 six +14 (5 better, 0 worse); 33 boards −3 (1 better, 10 worse), 3 clean against 5. Not adopted; hidden `--ramp-vias-fixed` | |
| §2.7 finer lattice | 2026-10-30 | 0.67 and 0.5 of the automatic pitch: −11 and −10 of 2,262 on 11 tier B boards, CPU +34 % and +117 %. Not adopted | |
