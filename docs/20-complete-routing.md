---
status: partly-built
code: [src/route/legalize.cpp, src/route/cut_test.cpp, src/app/route_job.cpp, src/analysis/escape_proof.cpp, src/drc/fit_rules.cpp, src/app/chains.cpp, src/passes/repair.cpp, src/route/obstacles.cpp]
reviewed: 2026-11-19
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

### 1.1 Two causes, told apart by the designer's layout (2026-11-19)

**The question** (2026-11-19): some boards never reach 100 %. Rules may be relaxed and compute spent. What is the cause?

**The designer oracle** (`bench/records/oracle-2026-11`). Each PCBench board has its designer's routing. Remove some nets' copper
from it and let the router put back only those nets, everything else fixed. Two kinds of board come out:

- **Search boards.** The nets the router leaves open from nothing all route once the rest of the board is the designer's
  (robomezzi, poncho, sd_wav, P8000, V2X, dorkyboard, mini_mass: 0 left; MonApollo 1 of 149, LFK78 2 of 115, TCKB 3 of 45). The
  router can draw every route. Its own other routes are in the way.
- **Modelling boards.** The router cannot put back even a random quarter of the designer's own nets (d20 60 of 117 left, fifogfx,
  Aleste, PCIE, sizif, E3, test-jig, EtherCAT, OtterPill, DronPi). The board as designed breaks something the router is held to.

**What the modelling boards break:**
- **Solder-mask openings that already expose another net's pad.** A card-edge connector with fingers on both sides, each opening
  both masks: every back finger lies in a front finger's opening, so no track may leave any finger (fifogfx, 40 of 104 connections
  open on the bare board). Solder jumpers drawn as mask graphics over two pads (test-jig, PCIE).
- **Minimums the design never had.** Every PCBench board is in the KiCad 5 format, with no copper-to-edge or hole clearance. KiCad
  10's defaults (0.5 mm, 0.25 mm) hold, and project rules are ignored on a board with legacy settings. The designers' layouts break
  them by the hundred.
- **Class values wider than the designer drew** (TCKB 0.406 mm against 0.25 mm on 90 % of the track length; OtterPill's vias).
- **Pin escapes with no slack.** sizif's supply pins on a 0.4 mm QFN leave with 0.2 mm tracks that fit only on the pad's centre
  line; the lattice misses it. One rule waived at a time shows the minimum track width (0.2 → 0.1 mm: 13 → 2 open), not the edge or
  hole clearance, is what frees them. This one is a lattice loss, not a broken rule.

**How much is which** (`tracemaker chains`, §2.9): of the 261 open connections of the best earlier results on 21 boards, 141 do not
route with every other route lifted (modelling), and 120 have blockers (search).

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

### 2.8 Rule fit: relax what the board's own copper already breaks (D194)

`route --fit-rules` (off by default; `src/drc/fit_rules.{hpp,cpp}`, `tracemaker fit-rules BOARD` prints the fit without routing).

- **Minimums.** The DRC runs on a reference board: the input, or with `--fit-rules-from BOARD` an earlier layout of it. For
  copper-to-edge clearance, hole clearance, minimum track width, annular ring, via diameter and drill, the rule comes down to the
  smallest value the reference's violations measure. The class clearance comes down to the low decile of the measured gaps, never
  under `--fit-clearance-floor` (0.09 mm): among thousands of pairs a few are slips. Fewer than three violations of a kind fit
  nothing.
- **Class values** (with a reference that has tracks): each class's track width comes down to the width that carries half of its
  track length on the reference, and its via to the reference's most used via of that class, if legal.
- **Mask openings.** A pad's solder-mask opening that already exposes another net's pad copper on that side no longer keeps tracks
  out (`Obstacles::set_soft_bridged_masks`). When openings drawn as graphics lie over pads, `--soft-mask-graphics` (D127) is turned
  on.
- **It only relaxes, and says so.** Every change is logged and written to the summary (`fitted_rules`). The output breaks the board's
  stated rules where the fit relaxed them, as the board or its earlier layout already did; KiCad's DRC reports those.

**Measured** on the 33 boards from nothing, 20 M work (`bench/records/oracle-2026-11` §4):

| | Open | Complete | Better / worse boards |
|---|--:|--:|---|
| base | 890 | 3 | |
| `--fit-rules` (the board's own pads) | 771 | 6 | 10 / 5 |
| `--fit-rules-from` the designer's layout | 734 | 8 | 15 / 3 |
| best of base and the fit to the designer's layout | 700 | 9 | |

fifogfx (40 → 0), test-jig (32 → 0), PCIE (30 → 0), ErgoDone (2 → 0), sd_wav (2 → 0) and, with the designer's layout, OtterPill
(11 → 0) complete; no earlier run had completed any of them. KiCad finds 0 unconnected and no short on each. The mask openings
close four of them and the class values OtterPill. The clearance helps in sum (838 against 852 open without it, before the mask
part) and hurts single boards (LeeChee 5 → 22, 58r-mxalps 0 → 5): a lower clearance makes the lattice finer (§2.7). poncho loses
12 with 110 pad openings soft; why was not looked into. The fit is therefore a rung that keeps the better result, not a default.

- **Open:** the copper-to-edge value is one number for the board, so fingers on the edge let tracks reach the edge anywhere
  (`--fit-edge-floor` bounds it). An escape with no slack (§1.1) is not a rule and is not fitted.

### 2.9 How deep a rip-up chain an open connection needs (`tracemaker chains`)

A measurement (`src/app/chains.{hpp,cpp}`). On a routed board, each open connection alone, the rest fixed:
1. **Blockers:** the nets the router's last rejected route would have ripped, or the nets its route on the board without other
   routes touches. The set is shrunk: a net goes back if the connection still routes.
2. **Joint:** the blockers are lifted within 1 mm of the connection's route and the router routes the connection and the blockers
   together.
3. **Chain:** failing that, the connection goes in and is held, the blockers route again, and a blocker that cannot becomes the
   next connection, to depth 4 (an ejection chain; Shin and Sangiovanni-Vincentelli, "A detailed router based on incremental
   routing modifications: Mighty", IEEE TCAD 6(6), 1987).

Every routing is the product router's, so a chain that closes is a legal board with one more connection.

**Measured** (21 boards, 261 open connections; `bench/records/oracle-2026-11` §5):

| | Connections |
|---|--:|
| no route with every other route lifted | 141 |
| with blockers | 120 |
| of those: one blocking net / two / three or more | 62 / 27 / 31 |
| closed joint | 17 |
| closed by a chain, depth 1 / 2 / 3 / 4 | 9 / 7 / 4 / 4 |
| not closed (still open at depth 4: 61; a lifted net left with no way: 16; too many nets: 2) | 79 |

- The median open connection is kept out by one net, and a third of them (41 of 120) close.
- Routing the connection and its blockers together is the step that works; fixing the connection first loses (10 of 43 closed
  on the 15 boards measured that way).
- **As a router step:** built into the repair stage and measured (§2.11). It does not beat the same run with more budget.

### 2.10 An exact model for ball grids (plan)

Under a 0.8 mm ball grid with the srj20 rules a gap between two balls holds one track, and an interstice holds a via or a track
crossing, not both (doc 05 §67: a cell's diagonal is 0.622 mm; a via and a track need 0.72 mm). The fan-out is then a small
discrete problem: about (n + 1)² interstices on two layers for about 90 nets. Negotiation finds a solution only by chance, and the
M26 candidates (reservations, costs, lanes, orders) all lost to it.

- **Model:** one variable per net, interstice and layer (CP-SAT, or SAT with reachability as in Bayless, Hoos and Hu, "Scalable,
  high-quality, SAT-based multi-layer escape routing", ICCAD 2016); capacity one per gap and per interstice; a via closes its
  interstice on both layers. Built as a relaxation of the real board (every real route maps to a model route), so "no solution" is
  a proof that two layers cannot work, and the answer to the user is to add layers (sample152 routes in 22 s on four).
- **Use:** a solution gives via sites and gap sequences to the router as a guide for the fan-out; the rest is routed as today.
- **Smallest experiment:** a Python prototype on one never-clean and one clean srj20 board.
- **Not built.** The literature search found no published result that closes the last 1-3 % of a board against an equal-budget
  negotiated router; this is a hypothesis.

### 2.11 Repair along the connection's route (D195)

The repair stage (`route --repair on`, doc 05 §75) lifts the unlocked copper round the two ends of an open connection and routes again.
Three options bring it to what §2.9 measured (`src/passes/repair.{hpp,cpp}`), all off by default:

- `--repair-select route|both`: the copper is looked for along the route the connection takes with the other nets' unlocked copper
  off the board, on that route's layers, at 0.2 and 1 mm from it. A connection with no such route is left alone after one run.
- `--repair-focus`: an attempt routes the connection's net and the nets it lifted, nothing else.
- `--repair-variants N`: N portfolio variants per attempt.

Measured after a 20 M run on the 33 boards (`bench/records/finish-2026-11`): the end windows close 28 of 887 open connections for
4.9 G expansions, the route selection 30 for 3.0 G, both 47 for 7.5 G; no board completes. The same run with three times the budget
(0.32 G expansions more over its eight variants at most) closes 159 and completes three more boards. **Not adopted as a step of the
default route.** It earns its place where little is open: inside the ladder of §2.12.

Without focus an attempt routes everything that is open. On d20 with 129 open every one of 126 attempts ended with more open than it
began. With focus CoreOne goes 14 → 11 and EtherCAT 20 → 15 at the same layers, where no unfocused attempt was kept.

### 2.12 A ladder on top: `route --finish` (D196)

Rule fit from nothing made some boards worse (§2.8). Routing only what is open, on the board the earlier run wrote, cannot: the routed
copper stays unless a repair window lifts it, and a window is kept only when fewer connections are open.

`route --finish` runs the rungs in turn, each on the output of the one before, and stops when nothing is open:

1. the run as given;
2. the rules fitted to the board as given (§2.8), with the repair of §2.11 (route and ends, focused);
3. with `--finish-masks`: pad solder-mask openings no longer keep tracks out (§2.13);
4. with `--finish-layers 2,4`: two, then four copper layers more than the board has.

The summary's `finish.rungs` lists every rung: open before and after, layers, the fitted rules.

Measured on the 33 boards with the rungs run by hand (open connections; boards complete):

| | base, 20 M | 60 M, default rules | fit from nothing | rung 2 | + pad masks (22 boards run) | + 2 layers | + 4 layers |
|---|--:|--:|--:|--:|--:|--:|--:|
| open | 890 | 731 | 771 | 597 | – | 296 | 274 (32 boards) |
| complete | 3 | 6 | 6 | 7 | 8 | 8 | 10 |

Aleste-520EX, which had 250 open on every earlier run, goes 195 → 8 → 0 (six layers). P8000 completes at +2 layers, MicroMaple at +4.
The ladder on top of the 60 M run completes ardfpga at rung 2 and MicroMaple at +2 layers; it was run on 14 boards before the runs were
stopped. Best arm per board, no designer layout used: **245 open, 16 of 33 complete**; KiCad 10.0.6 finds 0 unconnected on those
outputs it was given (12 of 13; the one with an unconnected item has a second complete output without).

As one command (`route --work 20000000 --finish --finish-masks --finish-layers 2,4`) on 23 of the boards (the six largest left out, one
stopped): 313 open become 56 and seven boards complete, KiCad 0 unconnected on each; eight more end 1 to 3 short.

What it does not do:

- **Memory.** mini_mass at six layers passed 100 GB in the plain run of its fourth rung and was stopped. The ladder writes its output after
  every rung it keeps, so a stopped ladder leaves its best board.

- **Order.** PCIE completes with the rules fitted from nothing and stays 1-2 short on top; OtterPill completes only from nothing with
  the designer's layout as reference. The ladder has the on-top order only.
- **Pins walled in by an earlier rung.** Added layers help a pin only if it can reach a via. On d20 (129 open, 27 pins walled in by
  tracks of the rungs before) two and four layers more change nothing.
- **Budget.** Three times the budget finishes three boards the ladder leaves one or two short. The two add up; which to spend first was
  not measured to the end.

### 2.13 Tracks through pad mask openings (D197)

A pad's solder-mask opening keeps other nets' tracks out (KiCad's `solder_mask_bridge`). Nine of the 33 designer layouts run tracks
through such openings: KiCad reports 3 to 118 bridges on them and none on the unrouted boards. On a board that opens every pad 0.2 mm
wider than its copper, the ways between the pads of a small part are all inside openings (memsarray's microphone: the supply pads
inside a ring of ground pads).

`--fit-rules-from REF` counts the pad openings of the reference with another net's track or via in them; with three or more, pad
openings no longer keep tracks out. `--soft-pad-masks` does the same by hand. memsarray completes for the first time; with the
designer's layout as reference on top of rung 2, sizif goes 38 → 26 and LFK78 31 → 27.

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
| §1.1 designer oracle | 2026-11-19 | two causes: search boards (the router's open nets route in the designer's layout) and modelling boards (the designer's own nets do not go back) | |
| §2.8 rule fit (`--fit-rules`, opt-in) | 2026-11-19 | 33 boards from nothing: 890 → 771 open, 3 → 6 complete (own pads); 734 and 8 with the designer's layout; six boards complete for the first time; 3-5 boards worse | D194 |
| §2.9 chain measurement (`tracemaker chains`) | 2026-11-19 | 261 open: 141 route on no board; of 120 with blockers 62 have one, 41 close (17 joint) | |
| §2.11 repair along the route, focused attempts (opt-in) | 2026-11-19 | after 20 M on 33 boards: 28 (ends), 30 (route, 39 % less work), 47 (both) of 887 closed, none complete; 3× budget closes 159. Not a default step | D195 |
| §2.12 `route --finish` (opt-in) | 2026-11-19 | rung 2 on top: 890 → 597 open, 7 complete; +4 layers 10 (Aleste 250 → 0); best arm per board 245 open, 16 of 33 complete | D196 |
| §2.13 pad mask openings from a reference (opt-in) | 2026-11-19 | 9 of 33 designer layouts have tracks in pad openings; memsarray complete, sizif 38 → 26, LFK78 31 → 27 | D197 |
