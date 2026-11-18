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

So bending 053 and 041 frees the site, and 043 walls in the pocket. 043's ball is the one just north of 055. Its B.Cu track runs down
beside 055's ball (x = 0.95, y -3.93 to -2.61), then south-east past (2.76, -0.78). With 043's track removed, the widest way south
narrows to a 0.35 mm gap at about (1.05, -1.15), which is west of 043's own route. One track fits through it.

Correction (2026-11-18): an earlier version of this note said the way south "is the corridor 043 uses". 043 does not pass through that
gap. The two-tracks test asks for both tracks in one corridor, which is stricter than needed: whether two separate ways south exist was
not tested geometrically. The routing experiment below is what shows that the router did not fit them.

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

**Outcome.** Freeing the via site is possible, but with the rest of the board fixed the router could not route 055 and 043 together
(both go from neighbouring balls to the bottom edge). A fix needs a wider rip-up than these four nets, not a local bend.

## Thinner tracks (`thin.py`)

Would narrower tracks for 055 and 043 let both through the corridor? The board's rules say no: `min_track_width` is 0.12 mm in its
`.kicad_pro`, so KiCad flags anything thinner. Measured anyway, with the clearance kept at 0.1 mm:

| Track width (mm) | One track out of the pocket | Two side by side |
|---|---|---|
| 0.12 | yes (to y = 6.06) | stops at y = -1.22 |
| 0.10 / 0.09 | yes | stops at -1.18 / -1.14 |
| 0.08 / 0.075 / 0.06 | yes | stops at -0.57 / -0.54 / -0.40 |

The widest way south has a gap of 0.348 mm edge to edge. It lies about 1.6 mm south of the ball, between net 017's diagonal B.Cu
track and net 002's via at (0.96, -0.78). Two tracks need 2w + 3 × 0.1 mm, so they fit only below w = 0.024 mm. The clearance, not
the track width, is what fills that gap. This shows only that the gap takes one track at any width a fab can make; as above,
it is not shown that 055 and 043 must both use it. No routing run was made.

## Larger rip-up (`big/`)

The same strip, with more nets: every net with B.Cu copper in a window from ball 055 down past the BGA's south edge (`W1.nets`: x -0.5
to 2.5, y -4 to 5.2, 27 nets; `W2.nets`: x -1 to 3, y -4 to 6, 36 nets). Then those nets are routed again with the rest fixed, 1800 s
and 8 threads. Arm R keeps the best board as it is and turns on the router's repair windows (`--repair on --repair-window-mm 2,4,6,8
--repair-work 20000000 --repair-branches 200`).

| Arm | Nets rerouted | Seed | Open (KiCad, whole board) | Added errors |
|---|---|---|---|---|
| W1a | 27 | 403 | 5 (055 and 3 others, plus 061) | none |
| W1b | 27 | 404 | 6 | none |
| W2 | 36 | 403 | 9 | none |
| R | repair | 403 | 2 (unchanged) | none |
| best board | | | 2 | |

Every rip-up lost routes: the more nets rerouted, the more stayed open, and 055 was open in every run. In arm R the main loop used the
whole 1800 s, so the repair windows never ran ("repair: skipped, --time-total (1800 s) is used up").

Arm R2 capped the main loop with `--time 120`, but the relax and ramp loop still used the whole 1800 s, so repair was skipped again. Arm
R3 ran the repair alone: the best board, `--repair on` with the same windows, and no relax or ramp (`big/cmd-R3.txt`). Every window
made things worse, and none was kept:

| Open connection | 2 mm window | 4 mm | 6 mm | 8 mm |
|---|---|---|---|---|
| 055 | 50 branches lifted, 2 → 13 open | 128, → 25 | 191, → 29 | 237 branches, over the 200 limit |
| 061 | 44, → 17 | 91, → 23 | 156, → 28 | 223, over the limit |

A window around a ball under this BGA lifts dozens of nets, and the repair search (20 M expansions an attempt) puts back far fewer than it
lifted. No rip-up tried here, from 4 nets to 191 branches, routed 055.
