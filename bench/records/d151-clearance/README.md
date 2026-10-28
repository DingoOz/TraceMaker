# D151 records: clearances in KiCad's order, copper graphics compared as KiCad compares them

Design: [docs/08-kicad-integration.md](../../../docs/08-kicad-integration.md) section 3.2, decision D151. Branch `m20-c5-fanout`,
2026-10-28. Judge: KiCad 10.0.6 (`scripts/kicad-cli`, Docker `kicad/kicad:10.0.6`).

## Files

| File | What |
|---|---|
| `vme-wren.json` | KiCad's and the engine's violation counts on vme-wren after D151 |
| `vme-jetson.txt` | `scripts/drc_parity.py` on vme-wren and jetson-agx-thor-baseboard |
| `demos.txt` | `scripts/drc_parity.py` on the other 15 KiCad demo boards |

## vme-wren

After D150 the engine's DRC still reported 425 clearance and 20 shorting errors that KiCad does not:

- **425 clearance**, all at 0.125 mm required. That is the board minimum; the custom rule `clearance_under_fpga` allows 0.1 mm in
  the `underFPGA` and `underDDR` rule areas. KiCad's `EvalRules` returns a matching custom rule's value before it applies the board
  minimum, so a rule may go below it. The engine floored every clearance.
- **20 shorting**, between copper lines and arcs of one footprint (the outline of each U.FL connector, drawn in copper).
  A footprint's copper shape has no net in KiCad (`PCB_SHAPE::IsConnected`), and nets are compared by pointer, so KiCad never tests
  two footprint shapes against each other, of one footprint or of two.

With D151 vme-wren matches KiCad on the 15 routing-relevant types `drc_parity.py` compares, and the engine reports no type more often
than KiCad. KiCad's remaining reports are checks the engine does not make: `length_out_of_range` (34, `fromTo()`),
`connection_width` (193), `silk_overlap` (199), `courtyards_overlap` (22), `zones_intersect` (12), `nonmirrored_text_on_back_layer`
(10), `padstack` (7) and `solder_mask_bridge` (6). jetson still matches.

## Other boards

- The other 15 demo boards: 12 match KiCad with the D149 and D151 builds alike. microwave's shorting reports go from 10 to 8
  (KiCad 6): two were between the copper polygons of two POLY footprints, which KiCad never tests against each other. The rest of
  microwave (clearance 2/0; pad-to-polygon reports that KiCad splits into shorts and clearance errors by which item has the larger
  UUID), One-Air-Max (clearance 14/12) and RoyalBlue54L-Feather (annular width 2/0, co-located holes 1/0) are as before.
- Of the 76 rule files among the fixtures, only vme-wren's has a clearance or hole rule below its board minimum. None of the boards
  with custom clearance rules has a pad or footprint clearance override. So no bench board routes differently.

## Rule parity

239 cases, all matching KiCad 10.0.6. New, each judged by KiCad:

| Case | What it checks | D149 build |
|---|---|---|
| `clearance_custom_below_minimum` | 0.1 mm gaps under a 0.125 mm board minimum; a 0.1 mm rule applies inside a rule area | fails |
| `clearance_local_over_custom` | a pad's 0.3 mm local clearance beats a custom 0.1 mm rule | fails |
| `hole_rules_custom_below_minimum` | 0.1/0.2 mm hole rules under 0.3/0.5 mm board minimums | fails |
| `hole_rules_minimum` | the same holes with no rule: the minimums apply | fails (names an NPTH hole "Hole") |
| `copper_graphics_compound` | two touching copper lines of one footprint, a line of a second footprint on one of them, and two touching net-less board lines | fails |

The engine now names a hole without copper "NPTH pad of <reference>", as KiCad does, and lists it in `hole_clearance` reports.
