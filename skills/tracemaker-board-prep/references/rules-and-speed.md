# Rules, net classes and routing speed

TraceMaker routes on a grid (lattice) and checks every step against the board's rules. Two settings
dominate its speed: the grid pitch, and whether rule checks can use per-net-class caches. Both are set by
the project files, not by the router's options.

## Grid pitch comes from the finest net class

```
pitch = clamp( min over every net class of
               (max(class track, board min track) + max(class clearance, board min clearance)) / 6,
               25 µm, 100 µm )   rounded down to 5 µm
```

Every class defined in `.kicad_pro` counts, whether or not a net uses it. Halving the pitch quadruples the
grid points per layer.

Measured on the StickHub demo (2 layers, fixed 2M work units, one thread): adding one unused 0.1/0.1 mm
class (with board minimums lowered to allow it) moved the pitch from 0.05 to 0.03 mm; the run took 1.7×
as long and routed 98 instead of 100 connections in the same work.

What to do:
- Delete net classes no net uses (KiCad keeps old ones after a schematic change).
- Keep fine classes (BGA breakout, RF) only if nets use them, and assign them by pattern so only those
  nets get them.
- Board minimums should be the fab's limits, not tighter "just in case": they also floor every class.
- `--pitch-um` overrides the automatic pitch; a coarser pitch is faster but can miss tight gaps.

## Custom rules: what keeps the caches

TraceMaker reads the board's `.kicad_dru`. Whether a rule is cheap depends on its constraint and condition:

| Rule | Router behaviour | Speed |
|---|---|---|
| `disallow track/via` with conditions on `NetName`, `NetClass`, `Type`, `Layer` | Enforced as per-net layer masks / via switches | Cached |
| `disallow track/via` with `insideArea`, `intersectsArea`, `enclosedByArea`, courtyard functions, `memberOfFootprint`, `Reference`, `Pad_Type`, `Width`, `Size_X/Y`, `Position_X/Y` | Enforced on every new track and via with its real shape, width, position and via span | Cached; one rule check per lattice point the search visits |
| `physical_hole_clearance` whose condition does not mention `NetName`, `NetClass` or `inDiffPair` | Enforced for every new via | Cached |
| `physical_hole_clearance` that mentions a net or net class | Enforced | **Disables caches board-wide** |
| `clearance` (any condition), and any other constraint type | Enforced exactly | **Disables caches board-wide** |
| `disallow hole/footprint/text` | Not checked by TraceMaker; KiCad DRC reports it | — |
| Condition that does not parse, uses a property TraceMaker does not evaluate (`Parent.Reference`, ...) or has a single number without units | **Not applied by the router or `tracemaker drc`** (KiCad still applies it); named in a `warning:` line. Preflight grades it `block` | **Disables caches board-wide** |
| Condition that calls a KiCad function TraceMaker does not evaluate (`memberOfGroup`, `hasNetclass`, `fromTo`, `getField`, `insideCourtyard`, ...) | **Kept, with that call taken as false**, so the rule misses whatever only the call selects and a `disallow` never fires through it (KiCad applies it in full); named in a `warning:` line. Preflight grades it `block` | As the rule's constraint |

TraceMaker evaluates `NetClass`, `NetName`, `Type`, `Layer`, `L`, `Reference`, `Pad_Type`, `Width`, `Size_X`,
`Size_Y`, `Position_X`, `Position_Y`, `isPlated()`, `existsOnLayer()`, `insideArea()`, `intersectsArea()`,
`enclosedByArea()`, `inDiffPair()`, `memberOfFootprint()` and the three `intersects...Courtyard()` functions,
as KiCad 10.0.3 does (checked against KiCad on a frozen corpus of rule cases). Front and back courtyards are
the footprint's own sides: for a footprint on the bottom, `intersectsFrontCourtyard` is its B.CrtYd outline.

Measured on StickHub (fixed work): one `clearance` rule conditioned on a single net made the run 2.15×
slower. The caches go for the whole board, not just the net the rule names.

What to do:
- Put clearances in net classes (a class per clearance need, assigned by pattern) instead of custom
  `clearance` rules.
- Keep-out areas: rule areas (zones with keep-out flags) are the cheapest; area and courtyard conditions are
  obeyed too, and keep net exemptions a plain keep-out cannot express.
- Run `tracemaker drc BOARD`: it prints a `warning:` line for every rule it does not apply. Those rules are
  still KiCad's to check after routing.
- Numbers in conditions are compared as KiCad compares them (exact doubles, units scaled, no rounding).

## Net-class patterns are cheap

KiCad 7+ assigns classes by name patterns (`netclass_patterns` in `.kicad_pro`). TraceMaker resolves each
net's class once when routing starts. Earlier versions matched patterns at every grid step: on a private
4-layer board that was about 80 % of the run time, and resolving once took a 20M-work route from about
140 s to 10 s (5M work: 32 s → 4 s) with identical output. On StickHub, two catch-all patterns now cost
about 15 %. Use patterns freely; that is the intended way to give groups of nets their own class.

## Via sizes

The via the router places for a class is

```
drill    = max(class via drill, board min through-hole diameter)
diameter = max(class via diameter, board min via diameter, drill + 2 × board min annular width)
```

When the class via does not fit, it falls back to a smaller "neck-down" via at the board minimums
(drill never below 0.2 mm). KiCad accepts it (it checks board minimums, not class sizes). If the class via
size is a hard requirement, raise the board minimums to it.

Blind/buried and micro vias are used only with `--blind-vias` / `--micro-vias`, only where a through via is
blocked, and only when the board settings allow them.
