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
