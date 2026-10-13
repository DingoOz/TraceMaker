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
