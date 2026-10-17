# EtherCAT shield: the designer's routing versus TraceMaker's, and what would close the board

Date: 2026-10-17. Board: `EtherCAT_shield_v1_EtherCAT_shield_v1` (2 layers, LAN9252 in a QFN-64 at 0.5 mm pitch, U1; headers P1 40 mm below U1,
P2 and P3 at the sides; GND pours on both layers). Files: `reports/realworld/boards/EtherCAT_shield_v1_EtherCAT_shield_v1/` (the designer's
routing in `human/`, the ladder and closure runs, the experiments of this note in `exp-qfn/`), renders in `reports/realworld/tmp_img/ec/`.

## 1. Test outcomes so far

KiCad's verdict under the board's own rules (0.2 mm clearance, 0.25 mm tracks, 0.6/0.4 vias, minimum via 0.4/0.3):

| run | what it did | open | compromises added |
|---|---|---|---|
| human | the designer's routing | **0** | 45: 37 vias 0.4/0.3 (0.05 mm ring, below the fab limit the ledger uses), 8 dangling stubs; 4 pad-to-pad clearances are the QFN footprint's own |
| ladder-A | T0 → T1 → T2 relaxation, designer's pours restored | 15 | 293 |
| ladder-O | own rules as T0 | 21 | 228 |
| ladder-S / S2 | + stitching vias / 200 M at T2 | 17 / 20 | 330 / 319 |
| ladder-L | + `--layer-assign` | 16 | 242 |
| ladder-R | + `--relax` | 30 | 194 |
| ladder-V | + `--via-in-pad` | 17 | 283 |
| ladder-G / K | supply rails strict / power and ground strict | 14 / 13 | 209 / 278 |
| ladder-W | fourth tier T3 (0.4/0.2 vias, experiment) | **9** (best) | 335 |
| closure-5 | relax only the nets in the way, shove, part moves, 5x work round | 19 | — |

Every open connection in every run ends at a U1 pin. The escape feasibility tool (`tracemaker escape`) says all 78 U1 pins can escape on
the 0.04 mm analysis lattice, so geometry and rules allow it. The part-move trials of closure-5 (C13, C14, C15, C6, C7, R15–R17 by
0.25–1 mm) changed nothing.

## 2. What the designer does

Measured on `human/routed.kicad_pcb` (1,268 tracks, 154 vias):

- **Straight, long escapes on the top layer.** 60 of 63 connected U1 pins leave on F.Cu, 0.25 mm wide (0.2 mm for the Ethernet pairs),
  straight out, and stay straight for 5 to 19 mm. The bottom side is a comb at 0.5 mm pitch running straight down toward P1 (pins 21–23 to
  the header's left column, 26–31 to its right column). Right-side pins (33, 34, 36, 39, 40) go out 0.7–2.5 mm, then turn down and run
  beside the comb for 10–23 mm. Left-side pins run straight left to P2 and J5 (SIO1: one 11.4 mm segment). Only 4 pins have a via of their
  own net within 1.6 mm of the pad.
- **The capacitors behind the pins are on the back layer** (C5, C6, C7, C15 at x = 138.1 mm, 0.3 mm behind the right pad row), so the
  top layer in front of the pins is free for the comb.
- **Supply pins go radial, the trunk stays outside.** The seven +3V3 pins (5, 7, 14, 20, 32, 37, 47) each run 0.5–1.7 mm out to a
  0.6/0.4 via; the +3V3 trunk runs on B.Cu about 2.5 mm outside the pad row, beyond the capacitor row, and 0.4–0.5 mm wide on F.Cu
  further out. The core net (`Net-(C12-Pad1)`, pins 6, 24, 38) is tied under the package on B.Cu (10 segments; C12 and C14 sit under
  the chip on B.Cu). 15 GND vias sit in the thermal pad.
- **Widths:** 0.25 mm on 698 of 1,268 segments, 0.2 mm on 281 (pairs and tight spots), 0.3–1 mm for power.

## 3. What the router does instead

- **Escapes bend early.** Its escapes turn 45 degrees after 1–2 mm toward the shortest path and cut across the neighbours' straight
  channels. The pins behind the cut are reported "boxed in (target)". The escape corridors of `--escape-plan` are 2 mm long; the designer's
  straight runs are 5–19 mm.
- **Supply nets hug the package.** The connection list is a spanning tree over pads, and for +3V3 and the core net its edges are
  package pins 1–3 mm apart (`U1.7 → U1.5`, `U1.14 → U1.7`, `U1.24 → U1.38`). The router routes these around the package on both layers,
  with vias in the other pins' escape zone. In closure-5, 10 of the 17 open pins had +3V3 copper 0.9 mm in front of them (renders:
  `tmp_img/ec/closure5_u1.png`, a +3V3 loop around U1 on B.Cu at about 1 mm from the pads).
- **Corridors are not re-reserved after a rip.** In `src/route/router.cpp`, `release_escapes` decrements `pad_open` when a pin's
  connection is routed and releases the corridor at zero; nothing increments it again when that connection is ripped up, only a full
  restart replans. Hence the many "ripped up by +3V3 and not routed again" lines.

## 4. Experiments (own rules, 20 M work, 8 threads, `--soft-zones`; the router's own counts, KiCad not run)

Input: `closure-5/stage-00-R/relaxed.kicad_pcb`. Scripts and outputs in `exp-qfn/`.

| run | change | routed |
|---|---|---|
| base | as closure round 0 | 177/207 |
| P50 | `--pitch-um 50`: the lattice divides the 0.5 mm pad pitch and U1's pads land on lattice points | 171 |
| F | `--first-nets` = the 49 signal nets on U1 | 176 |
| PF | both | 178 |
| E | `--escape-plan` on every portfolio variant | 178 |
| PFE | all three | 179 |
| S / SF | `--skip-net +3V3 --skip-net Net-(C12-Pad1)`, without / with signals first | 152/170 both: 18 still open at the same U1 pins |
| SEED3 | the first 3 mm of the designer's escape of every U1 signal pin fixed in the input | 182 (bottom and right pins still boxed in) |
| SEEDFULL | the designer's complete routes of the 49 U1 signal nets fixed in the input | 91/96 in 22 s; +3V3 pins 14, 20, 37 "enclosed by fixed copper" although the designer's vias fit there, off-lattice |

Reading: lattice pitch, ordering flags, the existing escape plan and even removing the supply nets do not fix it; 3 mm stubs are not
enough; once the comb exists the rest of the board is easy. The limit is the long parallel bundle toward one connector, plus the supply
nets occupying the escape band.

## 5. Suggested approaches, in order of expected payoff

1. **Bundle (river) escapes toward a common destination.** Detect runs of adjacent pins of a dense package whose connections go to the
   same part (here 16 pins to P1, 6 to P2, 4 to J3–J8) and route them as one bundle: parallel straight runs at the pad pitch, planar
   order, corridors reserved until the free channel ends or the bundle turns, then a fan-in. Starting points: the diff-pair coupled-section
   code (doc 05 §15) generalised to n tracks, or per-bundle global-route corridors (doc 05 §13).
2. **Radial supply pins on dense packages.** Penalise or forbid spanning-tree edges between two pins of the same dense package when the
   net has other copper within a few millimetres; each pin then drops to a via or capacitor and the trunk forms outside the escape band.
   Keep the trunk off the layer and band the escapes use (on this board: B.Cu beyond the capacitor row).
3. **Corridor persistence and length.** Re-reserve a pin's corridor when its connection is ripped; do not release a corridor while the
   pin's net still has open connections at that package; make the corridor length an option well beyond 2 mm (until fixed copper, or the
   bundle's turning point). Small change, directly addresses "ripped up by … and not routed again".
4. **Off-lattice via sites for short pin-to-via drops**, like the off-lattice escape stubs that exist for fine-pitch pads: the
   designer's supply vias sit 0.7–1.5 mm beyond the pad between the neighbours' tracks, where no lattice point fits a via.
5. **Back-layer tunnel under the QFN** for same-net supply pins (the core net under the chip on B.Cu, through the GND pour), lower
   priority.

Not worth more time on this board: a finer lattice, part nudges, further rule relaxation (ladder-W bought 9 open with 335 compromises and
0.4/0.2 vias; the designer meets the 0.2 mm rules with 0.25 mm tracks and no relaxation beyond the via ring).

## 6. Gotchas met on the way

- The designer's file is KiCad 5 format (`(module`, one-line segments); the fixtures and router outputs are KiCad 10 format with
  multi-line records; parse both with whitespace-tolerant patterns. Net `~SCS` is spelled `~{SCS}` only in KiCad's DRC output.
- `--skip-net` is repeatable (one net per flag); `--first-nets` takes a comma list and net names may contain parentheses and slashes.
- Seeding input copper that ends within the clearance of a pad makes that pad unreachable; cut stubs well short of other pads.
