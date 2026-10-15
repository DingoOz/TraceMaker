# Boards from the kistack skills

American Embedded's kistack skills (`npx skills add American-Embedded/kistack`) cover schematic,
footprints, layout review and exports. TraceMaker slots in between their layout and export steps:
kistack places, TraceMaker routes, kistack exports and reviews.

## What carries over unchanged

| kistack convention | Why TraceMaker benefits |
|---|---|
| `kicad-schematic`: power symbols for `GND`, `VDD`, `+3V3`, ... instead of labels | Plane and power nets get stable names, which net-class patterns and zones can target |
| `kicad-schematic`: differential-pair labels end in `+`/`-` or `P`/`N` | Exactly how KiCad and TraceMaker detect pairs (`--diff-pairs`) |
| `kicad-pcb` / `kicad-layout`: one canonical `.kicad_pcb`, one writer at a time, backups beside it | TraceMaker writes a separate output; validate it, then promote it to the canonical path with a backup |
| `kicad-pcb`: decoupling placed for minimum loop area, connectors at edges | Placement is the main input to routing quality; TraceMaker does not move parts unless asked |
| `kicad-export`: `kicad-cli pcb drc --refill-zones --schematic-parity --exit-code-violations` | The same refilled DRC is TraceMaker's sign-off; run it on the routed board |

## What to add before routing

- **Net classes by pattern.** kistack's schematic step names nets but does not set classes. Add classes
  for power, fine-pitch and impedance-controlled nets with `netclass_patterns`, and delete unused classes
  (see rules-and-speed.md: the finest class sets the grid).
- **Planes filled, teardrops off.** Refill zones and remove teardrops before routing.
- **Thermal reliefs** sized as in planes-and-thermals.md.
- **Board minimums = the manufacturer's limits** (the `kicad-export` step targets JLCPCB / NextPCB):
  they floor every class and every via.
- **Run the preflight** (`scripts/preflight.py`).

## Where the two disagree

`kicad-layout` tells agents to route by hand and to use an autorouter only for complex boards. When the user
asks for TraceMaker, use it for the bulk of the routing and keep the kistack review steps: render layers,
check return paths, and add ground stitching vias afterwards (TraceMaker does not place stitching vias, and
stitching vias placed before routing become obstacles). Critical nets (RF feeds, switching-regulator loops)
can be routed by hand first and locked; TraceMaker never moves locked copper.
