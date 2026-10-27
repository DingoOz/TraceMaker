# TraceMaker features

TraceMaker is an autorouter and component placer for KiCad, built to make the whole layout pass automatic and
trustworthy. Anything marked *(opt-in)* is off by default.

## Routing

- **Grid-based search:** routes on a fine grid, at 45° and 90° angles, with exact bend costs.
- **Exact rule checks:** nothing is added to the board until an exact geometric design-rule check passes.
- **Rip-up and reroute:** when connections fight over the same space, it removes and re-routes them, negotiating
  between nets, and learns from failures.
- **Eight strategies in parallel:** each board is routed several different ways at once and the best result is kept.
- **GPU guidance:** distance maps computed on the GPU steer the search, with an identical CPU version as a fallback.
- **Fast "impossible" detection:** a quick reachability check skips searches that provably can't succeed.
- **BGA escape planning:** reserves exit paths out of dense chip packages. A newer flow-based planner exists, but it
  routed fewer connections than the current one *(opt-in)*.
- **Vias and widths:** blind and buried vias, smaller vias where space is tight, and per-net-class widths and
  clearances.
- **Differential pairs:** USB, HDMI and Ethernet pairs are routed side by side at the rule's gap, with optional skew
  matching *(opt-in)*.
- **Length tuning:** meanders to bring tracks to a target length.

## Placement

- **Automatic placement:** an initial global layout, then legalisation (no overlaps) and annealing that shortens wiring
  and reduces crossings.
- **Routing-aware:** in its default mode it test-routes each candidate layout and keeps the most routable one,
  including the designer's own layout.
- **Gentle modes:** refine and ECO modes nudge an existing layout, for example shifting, rotating or swapping a few
  parts to fix routing failures.
- **Two-sided placement:** surface-mount parts can be flipped to the back, with mirroring that matches KiCad exactly
  *(opt-in)*.
- **Respects the designer:** locked parts, fixed edge connectors and keep-outs are never moved or violated.

## Component-aware rules

- **Part recognition:** spots common parts (USB, Ethernet, crystals, regulators, ESD protection, BGAs and more) from
  footprints and nets, with a confidence level.
- **Layout rules:** a catalogue of 193 rules drawn from manufacturers' application notes and datasheets: decoupling caps
  near their ICs, crystals near the MCU, ESD parts near connectors, keep-outs under magnetics.
- **On by default for placement:** placement pulls crystals and their load caps closer.
- **For routing:** the rules can produce a KiCad rules file and keep-outs *(opt-in)*.
- **Impedance:** a stackup and impedance calculator, and a user override file for any rule.

## KiCad integration and correctness

- **Native files:** reads and writes `.kicad_pcb` directly, and any part of the file it didn't change round-trips byte
  for byte.
- **DRC that agrees with KiCad:** its own design-rule check matches KiCad's verdicts, including on deliberately broken
  boards (264 of 264 test boards).
- **Plugins:** a KiCad plugin installable through KiCad's plugin manager, plus Python bindings.

## Determinism and visibility

- **Repeatable results:** a fixed work budget gives a byte-identical board on any thread count.
- **Live viewer:** a web view of the routing as it happens, with heatmaps of search effort and congestion.
- **Replays:** compressed recordings you can play back later.

## Benchmarks against Freerouting 2.5

Boards judged by KiCad's own DRC; the share of boards fully routed with no errors.

| Set | TraceMaker | Freerouting 2.5 |
|---|---|---|
| Tier A | 100% | 100% |
| Tier B | 67.5% | 50% |
| Tier C | 63.3% | 46.7% |
| Tier D | 50% | 36.4% |
| Held-out boards | 50% | 0% |

## Still open

- A global router that actually helps on very large boards.
- Crossovers for differential pairs whose P/N order swaps between the two ends.
- Testing the plugin inside a running KiCad.
