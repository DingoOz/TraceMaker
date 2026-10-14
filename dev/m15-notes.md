# M15 working notes (raw numbers; the write-up goes into docs/16 §12)

## Protocol
srj20-gap = 57 boards (P1 misses that tscircuit solves). 30 M work, one variant, no GPU, fast bends, automatic breakout settings. Baseline gap20-auto: 23/57 clean (40.4 %), completion 90.9 %.
srj20-p2fail = the 76 boards that P2 (50 M, one variant) leaves open.

## Lattice pitch (equal work, 30 M)
auto 35 um: 23 clean; 36.7: 18; 44: 26; 45: 21; 55: 20. Union of the five runs: 31. Spread of +-3 boards is run-to-run noise from chaotic negotiation: pitch is neutral.
Reasoning that motivated it: track + clearance = 0.22 mm; 35 um lattice needs 7 cells = 0.245 mm (11 % waste in parallel runs); 44-45 um needs 5 (0-2 %).

## Transactional negotiation (--txn)
A negotiated route that rips victims is kept only if the victims can be re-routed (strict search) and the net change in routed connections is >= 0
(ties kept with --txn 1, strict gain with --txn 2). Rollback restores a whole-board snapshot.
sample069 30 M: plain 41/53, txn1 50/53, txn2 46/53.
Gap set, 30 M: txn1 12 clean (21 %), completion 94.9 % (plain 90.9 %); txn2 4 clean, completion 90.4 %. Mean open connections 5.1 -> 2.8; 14 boards end with exactly 1 open.
Annealing acceptance (--txn-temp T: keep a net loss of k with prob exp(-k/T)): T 0.25/0.5/1/2/4: clean 30/32/40/42/40 %, completion 95.2-95.6 %. Cooling (T falls to 0 over the budget) T=1: 35 %, T=2: 39 %.
Gap set, 100 M: plain 28/57 clean (49.1 %, completion 93.9 %); txn1 T2 cooled: 32/57 (56.1 %, completion 97.8 %).
No DRC error added in any run.
Victims per transaction on sample123: 1-10; failures to re-route 0-5.

## More 30 M results on the gap set (57), txn base = --txn 1 --txn-temp T
cap 20k/60k victims re-route cap: 36.8 % clean (no gain). nested depth 1/2: 24.6 % / 22.8 % clean (worse: fewer passes per budget).
reach-check 2 + flood-work-pct 25 + txn T1: 30/57 clean (52.6 %), completion 96.3 %, 137 s CPU per board (txn T1 alone: 23/57 in 106 s; plain 23/57 in 71 s).
## 100 M, gap set (57)
plain 28 clean (49.1 %); txn T2 cooled 32 (56.1 %); T1 cooled 30 (52.6 %); T4 cooled 34 (59.6 %); T2 not cooled 36 (63.2 %), completion 97.8-98.0 %. Cooling hurts at long budgets.
## srj20-p2fail (76 boards P2 leaves open)
100 M plain: 5 clean, mean completion 80.2 %; 100 M txn T2 cooled: 9 clean, completion 89.0 %.
30 M, txn + rc2 + fw25 + prefill + cache: T1 7 clean (9.2 %), T2 cooled 3, via cost 1.5: 3, soft cost 3: 4; completion 85.4-86.4 %; 85 s CPU per board.
Union of all runs on the 76: 13 clean (incl. the M14 16 x 50 M portfolio's 9). Best open count over all runs per board: 13 x 0, 8 x 1, 6 x 2, 4 x 3...; boards of 70+ connections stay >= 8 open.
## Engine
Field cache (exact, byte-compare of inputs) + prefill of the fixed-copper cache: identical output on sample123, 38 % less wall time (hits 236 of 300 fields); changes 6 of the 30 quick boards (heuristic tie-breaks), so it is applied only on breakout boards.
gprof (txn run): search() 26 % self, field_cpu 20 %, routed_copper_part 16 %, point_state 7.5 %.
Tidy sweep (re-route every routed connection alone, keep if shorter): 10-20 connections shortened per sweep.

## Later experiments (30 M, srj20-p2fail, txn profile, one variant)
net order 1 / 2 / 3: 5 / 7 / 10 clean (base order 0: 7); completion 80.3 / 85.9 / 86.0 % (86.3 %). stall restarts 3 / 6 passes: 3 / 5 clean. no strict first pass: 7. crossing cost 0.5: 4 clean, completion 84.3 %. orders 2 / 4 victim orders: 4 / 1 clean.
tidy sweeps 1 / 2: 3 / 5 clean. non-cooled T 2/4/8/1000: 6/3/2/5 clean.
Full srj20 (200), one variant 30 M with the profile: 129 clean (P2 at 50 M: 124), 59 s CPU per board (67); lost sample014, sample123; gained 7.
Full srj19 (200), one variant 30 M with the profile: 178 clean (P2: 181), 32 s CPU per board (46); completion 99.07 % (98.63 %).
Portfolio, 39 boards of the 76: designed list + profile 11 clean / 342 open; breakout list 12 / 319; breakout list + 4 rounds 12 / 284.
testing budget: 3 h from 05:53 (user, 2026-10-13)

## 2026-10-13 plateau diagnostics (3 h testing budget, ended 08:53)
- Slack test: 6 stuck boards (039, 048, 071, 072, 103, 104) re-routed with clearance 0.08 mm instead of 0.1 (until-complete, 30 min each, 16 variants): open 2, 1, 2, 5, 6, 3 (at 0.1 after 3 h: 4, 1, 3, 2, 5, 2). None completed with 20 % more room, so the plateau is not a packing-density limit (hex/any-angle lattices would not help).
- Open balls are not sealed: of 8 open connections on near-complete boards, the four adjacent via sites are free (0-1 occupied) and the top corridors lightly used. The partner is 11-16 mm away; the blockage is along the path, not at the ball.
- `--lns-open 200 --lns-open-radius-mm 3.5` resumed from saved state (sample103 72/77, sample104 70/72, 6 rounds x 20 M, 4 variants): no change (identical counts to the plain resume).
- Best over all runs (files): 167 srj20 boards complete; 3 boards 1 open, 21 boards 10+ open.
- Research: research/bga-plateau-2026-10.md (ranking: route-pool exact selection first; hex/Y/any-angle 0).
diag-c006 started 09:58: clearance 0.06 doubles corridor capacity (2 tracks fit in 0.44 gap: 0.24+0.18); tests whether array-interior capacity binds
- diag-c006 (clearance 0.06 mm, until-complete, 30 min cap): ALL six stuck boards complete (048 in 184 s, 071 558 s, 072 944 s, 039 1475 s, 103 1526 s, 104 1635 s). At 0.08 mm none did. At 0.06 two tracks fit in the 0.44 mm pad gap (0.24+0.18) and between vias (0.8-0.3=0.5 gap): discrete capacity, not density, is the limit. 0.08 doubles only the via-gap (0.48<=0.5), not the pad gap (0.48>0.44): the pad-gap (top-layer corridor) capacity looks like the binding resource. diag-v025 (via 0.25/0.12) started to test the bottom/via side.
- diag-v025 (via 0.25/drill 0.12, clearance 0.1; via gap 0.55 holds 2 tracks) and diag-p026 (ball pads 0.26 instead of 0.36; pad gap 0.54 holds 2 tracks), same six boards, 30 min each, until-complete: routed 435/453 (via 0.25: 73/74 76/81 76/78 68/71 71/77 71/72) and 421/453 (pads 0.26: 72/74 74/81 72/78 66/71 68/77 69/72), no board complete; clearance 0.08: 434/453; 0.06: 453/453. So doubling only the pad gap or only the via gap does not help; the clearance step 0.08 -> 0.06 does. 0.06 also lets a via sit in a pad gap (0.15+0.06+0.18 = 0.39 <= 0.4) and doubles every track-track spacing, so the jump is not one resource; a single-resource cut model would not explain it. (Run-to-run noise on this plateau is about +-3 connections per board.)
- Pads are 0.36 mm squares (rect), not discs, in the srj problem files; the pad gap 0.44 holds one track at 0.12/0.1 either way.
- Next: clearance continuation (route at 0.06, tighten 0.07/0.08/0.09/0.10, each stage warm-started from the previous routes that are still legal: `--state-in ... --state-recheck`), driver scratchpad/cont.py.

## 2026-10-13 evening: clearance continuation
- Plateau runs do not drift: resuming sample103 (72/77) for one more round of 8 variants x 20 M gave 72 routes identical to the input (the round returns the best state seen, which is the start); the variants' own end states are 53-71 (mid-restart), so `--island-slack` (continue from a variant's own end state when within the slack of the best) has little to continue from.
- Continuation (external driver scratchpad/cont.py, then `--ramp-from`): sample048 from the 0.06 solution: complete at 0.07 (572 s), 0.08 (181 s), 0.085 (136 s, from the 0.08 state), 72/74 at 0.09 (1070 s); from scratch at 0.08: 73/74 after 30 min. sample071 complete at 0.065 (189 s), 70/71 at 0.07 (753 s). sample072 80/81 at 0.065, 76/81 at 0.07 and 75/81 at 0.08 (15 min each). So a board can be completed at a clearance where scratch search fails, and the boards differ in how far they climb (048 >= 0.085, 071 about 0.065, 072 and 039 below 0.065).
- The 0.085 solution of sample048 (gaps.py): top pad gaps with 1 track 124 of 288 (none with 2), bottom via pairs with 2 tracks 4 of 218; violations at 0.10 (viol.py): 14 via-pad, 31 track-via, 48 track-track, 36 track-pad, all with gaps 0.087-0.098 (lattice squeeze), so it is a few corridors short, not a different regime. Pad-gap capacity doubles at c <= 0.0667 (0.24 + 3c <= 0.44), via-gap capacity at c <= 0.0867 (0.24 + 3c <= 0.5) for 0.3 vias at 0.8 pitch.
- Retained routes after the tightening (saved 0.06 routes legal under the new rules, sample072/048): 0.07: 57/58 of 81/74, 0.08: 56/54, 0.10: 12/11; a greedy minimum-conflict subset keeps one more than first-come (56 -> 57), the loss is mostly routes that violate fixed copper (pads), not each other.
- Native ramp run `ramp1` (6 stuck boards, 3600 s, 16 variants, from 0.06 in steps of 0.005, 3 rounds per stage): in progress.

## 2026-10-13 night: relax, route, legalise (the plateau is not capacity)
- **Result.** A gridless legaliser (vertices of the routed tracks and vias moved, topology fixed, until every clearance of the true rules holds) turns the complete 0.06 mm solutions of the stuck boards into legal 0.10 mm boards: LP prototype (scipy/HiGHS, scratchpad/legalize.py) 071, 039, 104 with 0 violations and KiCad DRC "0 router-introduced errors, 0 unconnected" (74/74 sample048 from the 0.085 solution too); 103, 048, 072 keep 3-6 violating pairs. Position-based dynamics (no LP; pbd.py, then C++ `route/legalize.cpp`) gives 071 and 104 at 0, 039 at 2 pairs, 103 at 5-9, 048 at 4, 072 at 8-9. First complete boards at the true rules among those that stayed open for hours of scratch search: sample071 (0.22 s from the 0.06 mm solution), sample048.
- So the "discrete capacity binds" reading of the 0.06 mm result was wrong: the lattice router (0.035 mm pitch, tracks only on lattice points, committed routes immovable) wastes room it cannot use; the 0.06 mm solutions have the topology of a legal 0.10 mm board. The 0.06 mm routing completes in minutes because the lattice has room; moving vertices off the lattice recovers the rest.
- Pipeline: `tracemaker route BOARD --warm-pcb RELAXED.kicad_pcb --legalize ...`: tracks and vias of RELAXED are put into the connections (nets with one connection), moved by `legalize_routes` under the board's rules, those still legal under the exact check are kept (min-conflict subset), the rest is routed again by the portfolio. `--ramp-from C0 --ramp-step S --legalize` does the same between ramp stages.
- sample103 with the LP-legalised board stripped of its 3 violating nets: 74/77 and 0 DRC errors; the portfolio (20 rounds x 20 M x 16) could not route the 3 (blocked by the legalised neighbours).
- Violations of the 0.085 solution of sample048 at 0.10 were all 0.087-0.098 mm squeezes (131 pairs), none a capacity cut: no double-used pad gap, 4 double-used via gaps that are 0.9+ mm apart.
- Stopped by decision (user: enough testing) at 2026-10-13 23:18: relax1a/1b/1c (27 open srj20 boards, `--variants 16 --relax`) after sample095, 031 (relax1a) and sample159 (relax1b) had finished clean; sample104 (R4, 71/72) and the rest were killed. 072 completed in R3 (81/81, DRC clean) after about 41 min of breathing cycles; 104 never did (R1-R4: 71/72).
- Negative on the last missing connection: islands (`--island-slack 1` with the ramp), 0.01 mm ramp steps, half-pitch repair lattice (`--pitch-um 17.5`, 6 rounds, 71/72), 3,000 projection sweeps (pairs left 1/0/8/3/7 instead of 1/2/9/4/8).
