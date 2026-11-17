# Fan-out first, then kept

A dog-bone fan-out is written into the board as fixed copper before routing (`fanout.py in.kicad_pcb out_dir skip`): from each netted
ball a 0.12 mm F.Cu stub to the diagonal site half a pitch out toward its quadrant's corner, and a 0.3/0.15 via there. `skip` rings
(counted from the outside) are left without a fan-out so they can escape on F.Cu. It cannot be done with reserved vias inside the router,
because a via's hole_to_hole check includes holes of its own net. The router then routes from the vias (`fan.py ARM board...`, the
M26 recipe, 3300 s, 6 threads, seed 403). KiCad adds no error on any board (the fan-out vias are not dangling once routed).

| Board | k0 (all balls) | k1 (outer ring free) | k2 (two rings free) | Control, from nothing | Best ever |
|---|---|---|---|---|---|
| sample152 | 16 | 12 | 12 | 7 | 2 |
| sample070 | 19 | 15 | 16 | 7 | 2 |
| sample040 | 20 | 18 | 15 | 6 | 2 |

Every arm leaves about twice as many open as the control. The likely cause (not measured): a fixed via under every inner ball blocks one B.Cu channel in two between
via columns; the router's own solutions put fewer vias under the package and use the F.Cu space between balls. Not adopted.
