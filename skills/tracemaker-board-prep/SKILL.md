---
name: tracemaker-board-prep
description: Prepare a placed KiCad 10 board for TraceMaker autorouting so it routes fast and legally. Checks net classes and lattice pitch, custom .kicad_dru rules that slow the router, ground planes and zones, thermal reliefs, teardrops, via sizes, differential-pair names and dense-package escape, with a preflight script. Use after placement (for example after the kistack kicad-layout / kicad-pcb skills) and before running tracemaker-route.
---

# Preparing a board for TraceMaker

## For humans

TraceMaker routes whatever board it is given, but how the board is set up decides how fast it routes and
whether KiCad accepts the result. A few settings that cost nothing in KiCad cost a lot in a router: one
fine net class nobody uses shrinks the routing grid for the whole board; one custom `clearance` rule
turns off the router's per-class caches. Stale zone fills and leftover teardrops change what the router
thinks is connected. This skill finds those and fixes them before routing.

Placement is not part of this skill. Place first (kistack `kicad-layout` / `kicad-pcb`), then prepare,
then route with `tracemaker-route`.

## For agents

Work on the project's canonical `.kicad_pcb` with its `.kicad_pro` (and `.kicad_dru` if any) beside it:
TraceMaker and KiCad read net classes, board minimums and custom rules from those files, and without them
route on defaults. If the user has the board open in KiCad, ask them to save first, and after any scripted
edit tell them to close without saving and reopen (or File → Revert), or KiCad overwrites the change.

### 1. Run the preflight

```
python3 <this skill>/scripts/preflight.py BOARD.kicad_pcb [--tracemaker PATH] [--json preflight.json] [--soft-zones] [--small-pad-mm MM]
```

It reads the board, project and rules, runs `tracemaker drc` (which rules TraceMaker applies) and
`tracemaker escape` (dead pins, with the zone and via options of the route it suggests) when the binary is
found (`--tracemaker`, `$TRACEMAKER` or `PATH`), and prints findings graded `block` (routes wrong or
illegal), `slow`, `quality` and `info`, then a suggested route command.
Fix every `block`. Fix `slow` and `quality` findings unless the design needs them; say which you kept
and why.

### 2. Fix blocks

| Finding | Why it matters | Fix |
|---|---|---|
| No `.kicad_pro` beside the board | Net classes and board minimums are missing; the router warns and uses zeros | Keep the project files with the board |
| Unfilled zones (no `filled_polygon`) | Connectivity is computed from fills; the router sees planes as absent | Refill all zones (`B` in KiCad, or `kicad-cli pcb drc --refill-zones --save-board`) and save |
| Teardrop zones | Teardrops are track copper KiCad generates; left behind after deleting tracks they are stray pad copper that changes what counts as connected (complex_hierarchy keeps 165 of them) | Remove teardrops before routing; regenerate them after |
| Custom rule TraceMaker does not apply (condition does not parse, uses a term it does not evaluate such as `memberOfGroup` or `Parent.Reference`, or has a lone number without units) | Neither the router nor `tracemaker drc` applies it, KiCad still enforces it, and it disables caches. `tracemaker drc` names each one in a `warning:` line | Fix the condition |
| Open Edge.Cuts outline | Board area and edge clearance are undefined | Close the outline |

Also check, though they are graded `quality`: **dead escape pins** from `tracemaker escape`: pins of dense
packages with no way out on the analysis lattice under the board's rules. Pins whose net the existing copper
already completes are counted as connected and not searched, so there is no need to strip a routed board.
A pin "walled in by custom disallow rules" escapes without the rules: relax the rule for it or fan it out by
hand. Others are usually a real placement or fan-out problem, occasionally routable off the lattice; the
router still tries them.

**After deleting routing, the old zone fills are traps.** Their voids hug the removed tracks, so with fills
as fixed copper only the old geometry fits. Route such boards with `--soft-zones` and analyse them the same
way (the preflight's `--soft-zones`). On stripped CM5, `tracemaker escape` reports 18 dead pins with the fills
fixed and none with `--soft-zones`.

**Position, size and footprint `disallow track/via` rules** (`insideArea`, courtyard functions, `Width`,
`Position_X/Y`, ...) are obeyed by the router on every new track and via; they cost some search time but keep
the caches. Rule areas (keep-out zones) remain the cheapest way to fence off an area for every net.

### 3. Fix speed findings

Details and measurements: [references/rules-and-speed.md](references/rules-and-speed.md).

- **Finest net class sets the grid for the whole board.** The routing pitch is (track + clearance) / 6
  of the finest class, between 25 and 100 µm. Delete unused classes; keep fine classes only for the
  nets that need them, assigned by pattern.
- **Custom rules other than `disallow` and net-independent `physical_hole_clearance`, and rules TraceMaker
  does not apply, turn off the per-class caches for every net.** One custom `clearance` rule made a demo
  board route 2.15× slower. Express clearances as net-class clearances where possible.
- **Net-class name patterns are fine.** They are resolved once per net (this was the large speed-up on
  a private 4-layer board: 20M-work route 140 s → 10 s); use them freely.

### 4. Fix quality findings

Details: [references/planes-and-thermals.md](references/planes-and-thermals.md).

- **Ground and power planes.** Give each reference plane a full-board zone on its own inner layer,
  filled. Zones are obstacles unless the route uses `--soft-zones`, which lets the router cut them and
  connect pads straight into them; plan which mode the board is for.
- **Thermal reliefs.** The router does not model thermal-spoke starvation, so KiCad can report
  `starved_thermal` after the refill. Give zones a thermal bridge width at least the class track width,
  a modest thermal gap, and use solid connections for large power pads where assembly allows.
- **Vias.** A board minimum via diameter above the net-class via silently enlarges every via (one demo:
  0.6 mm class → 1.5 mm vias). Set board minimums to the fab's real limits.
- **Differential pairs.** Pairs are names that differ only in a final `P`/`N` or `+`/`-`
  (`USB_D+`/`USB_D-`, `ETH_TXP`/`ETH_TXN`). `USB_DP`/`USB_DM` are not pairs to KiCad or TraceMaker; rename
  in the schematic and update the PCB.
- **Small SMD pads.** With planes, plan to route with `--keep-vias-off-pads` (pads narrower than 2 mm;
  `--vias-off-pads-below MM` for another limit); without it soft zones put vias into small pads.
- **Locked items** are never moved or ripped. Lock only what must stay; existing unlocked routing is kept
  and routed around.

### 5. Record the baseline

Run KiCad's DRC on the prepared, unrouted board and keep the report: errors already present (footprint
issues, silk) are not the router's, and sign-off compares against this.

```
kicad-cli pcb drc --refill-zones --format json --severity-all --all-track-errors -o build/prep-drc.json BOARD.kicad_pcb
```

Then hand over to `tracemaker-route` with the preflight's suggested command.

## Boards from the kistack skills

Boards made with American Embedded's kistack skills (`kicad-schematic`, `kicad-layout`, `kicad-pcb`,
`kicad-export`) need little preparation; see [references/kistack-handoff.md](references/kistack-handoff.md)
for what carries over and the few things to add.
