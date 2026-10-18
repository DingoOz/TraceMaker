# Reproductions of router limits found in the real-world study

## e3-u7-boxed-in.kicad_pcb (explained 17:05: net-less copper graphics)

`Hardware_Playground_E3_Pcb_rev0.3` with every footprint except U7 (an EasyEDA MP1584 module) and its GND pours removed. Net `+VB` is the two pads `U7.2`.

    tracemaker route e3-u7-boxed-in.kicad_pcb -o out.kicad_pcb --only-net +VB --work 300000 --threads 1 --no-gpu --no-kb
    -> routed 0/1 (U7.2 -> U7.2 boxed in)
    tracemaker route ... --soft-copper-graphics
    -> routed 1/1

Cause: the board file draws net-less copper polygons (`gr_poly` on F.Cu and B.Cu) over the pad area; the router treats board copper graphics as fixed copper that nothing may touch. `--soft-copper-graphics` (doc 05 §48, D127) ignores them.
(Moving the module away from the polygons also made it route, which was the clue.)
