# sample152: fixing bga_conn_055 by hand-picked rip-up

Best board: `srj20-again/sample152/r1/a1` (2 open: 055 and 061). Ball 055 is two rings in on the north side, at (0.8, -3.2) from the
BGA centre. Its test point is at the bottom edge (2.03, 10.74). The board files are not here (srj20 has no licence).

## Analysis (`n55.py`, `p3.py`, `n55.png`)

All four of 055's dog-bone via sites are blocked. The south-west site (0.4, -2.8) is blocked only by two B.Cu tracks running
north-south: 053 runs through it (0.04 mm from its centre) and 041 sits 0.28 mm from it where 0.31 mm is needed. Both end at their
own vias just south of the ball.

A flood fill on B.Cu at 10 µm with KiCad's clearances tests where a track from that via could reach:

| Moved (vias kept) | Furthest south reached |
|---|---|
| nothing | site blocked |
| 053 and 041 tracks | y = -2.03 (closed pocket) |
| + 043 track | y = 6.06 (out of the BGA) |
| + 043, with room for 043 alongside 055 (south of y = -1.5 / 0 / 2) | -1.22 / 0.0 / 3.53 |

So bending 053 and 041 frees the site. The only way south is the corridor 043 uses, and it holds one track, not two.

## Experiment (`strip.py`, `fix.py`)

Strip every track and via of 055, 053, 041 and 043 from the best board. Then route only those four nets (`--only-net`) for 900 s
with the M26 recipe, 8 threads; everything else is fixed.
- **Arm A** pre-places 055's dog-bone: an F.Cu stub plus a via at (0.4, -2.8).
- **Arm B** does not.

| Arm | Seed | Open (KiCad, whole board) | Nets left open | Added errors |
|---|---|---|---|---|
| A | 403 | 4 | 041, 055, 043 | none |
| A | 404 | 4 | 053, 055, 043 | none |
| B | 403 | 3 | 055, 043 | none |
| B | 404 | 3 | 055, 043 | none |
| best board | | 2 | 055 (and 061) | |

055 was routed in no run. 043, which was routed in the best board, was open in all four. In arm A, the fixed via makes it worse:
a third net is lost. The repair windows did not run (the time limit was used up).

**Outcome.** Freeing the via site is possible, but 055 and 043 both need the one B.Cu corridor south. With the rest of the board fixed,
the router found no second way for either. A fix needs a wider rip-up that gives 043 (or 055) another corridor south, not a local bend.
