# Planes, zones and thermal reliefs

## How TraceMaker sees zones

- **Default (hard zones):** zone fills are fixed copper. A foreign-net fill blocks tracks and vias; a fill
  is not a routing target. Pads already joined to a fill count as connected.
- **`--soft-zones`:** fills are copper KiCad will refill, so the router may cut through foreign fills
  and connect SMD pads straight into a plane with a via. Rule areas, holes, board edges and locked copper
  stay hard. The output keeps the old fills, now stale: refill before judging. KiCad's refill decides final
  plane connectivity.

Connectivity at the start comes from the fills in the file. Refill every zone and save before routing;
an unfilled zone looks like no plane.

## Ground and power planes

- One reference plane per inner layer where the stackup has one (4 layers: signal / GND / power or GND /
  signal is common), as a zone covering the whole outline, filled.
- Keep plane nets in their own net class only if their track width differs; planes do not need custom
  rules.
- Zone priority: higher priority fills first where zones overlap; make it intentional.
- Split planes: the router treats each zone separately; a track over a split loses its return path.
  Prefer solid reference planes under fast signals.

Measured on the KiCad demos and a synthetic plane board (3M work, one run each, KiCad DRC after refill;
unconnected items left):

| Board | Fills fixed | `--soft-zones` |
|---|--:|--:|
| RoyalBlue54L-Feather (8 layers) | 118 | 63 |
| plane_smd (4 layers, all SMD) | 6 | 1 |
| StickHub (2 layers) | 30 | 15 |
| multichannel_mixer (2 layers) | 11 | 4 |
| complex_hierarchy (2 layers) | 18 | 17 |
| interf_u (2 layers) | 34 | 33 |
| pic_programmer (2 layers) | 6 | 7 |
| sonde xilinx (2 layers) | 0 | 2 |

Decide per board, and add `--keep-vias-off-pads` with soft zones, or vias land in small SMD pads.

## Thermal reliefs

TraceMaker does not model thermal-spoke starvation: after the refill, KiCad reports `starved_thermal` when
fewer than the board's `min_resolved_spokes` spokes survive around a pad (new tracks and vias can block
spokes). The demo benchmark hit this on interf_u (1–3 per run).

Reduce the risk before routing:
- Thermal bridge (spoke) width ≥ the net class track width, and a thermal gap no larger than needed.
- `min_resolved_spokes`: 2 is KiCad's default; it is a design decision, not something to lower to pass DRC.
- Large power or ground pads (regulator tabs, exposed pads) on solid connection if assembly allows.
- After routing, `starved_thermal` in sign-off means re-routing near that pad or a manual fix.

## Teardrops

Teardrops are zones KiCad generates from tracks (`(attr (teardrop ...))`). When the tracks they belonged to
are deleted, they remain as stray copper on pads and vias, and with `--soft-zones` they look like fills. Remove them before routing (KiCad: Edit → Remove
Teardrops) and add them again after routing. The complex_hierarchy demo, stripped of its tracks, keeps 165 of
them on its pads.

## Keep-outs and rule areas

Rule areas (zones with keep-out flags) are read with their own flags: tracks, vias, pads, copper pour and
footprints are independent. Use them for areas the router must avoid (antenna, under crystals, mounting
holes). They are cheaper than `insideArea()` rule conditions, which the router also obeys.

## Solder mask and logos

The router ignores unfilled circles on mask layers (for example inside logo footprints); KiCad can report
`solder_mask_bridge` when a track passes under one. Mark logos' area with a rule area if routing near them.
