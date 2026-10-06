# Overnight report: closing M8 and the rest of the roadmap

Written during the unattended run that started 2026-10-06 11:10 UTC. Everything is committed locally on `main`
(not pushed). Assumptions taken without being able to ask: `dev/assumptions.md` A35–A43, summarised at the end.

## Summary

Every task in `dev/progress.json` is now either built and measured or closed with the measurement that says why it
is not built (my reading of "100 % complete", assumption A35). That is not the same as every gate being met:

| Milestone | State | Gate |
|---|---|---|
| M3 viewer | closed | replay gate met; 60 fps not measured (no GPU-backed browser here) |
| M6 global routing + GPU | closed (earlier in the day) | met for the GPU workload routing uses; global routing itself did not help |
| M8 placement E–G + ECO | closed | **not met**: from-scratch clean pass 85 % on the held-out set, gate 90 % |
| M9 escape planning | closed | **not met**: 41 % of the dense-package boards clean (Freerouting 2.5: 24 %) |
| M10 portfolio | closed | met (determinism); successive halving built, measured slightly worse, off |
| M11 KiCad integration | closed | **met**: round trip in a running KiCad GUI, done headless |
| M12 advanced rules | see below | per-feature |
| M13 component rules | closed | net classes from rules now reach the router |

Three things in this run changed what the product does rather than what the roadmap says:

1. **Two router correctness faults**, found by held-out boards and present with human placements too:
   copper was routed across Margin-layer lines, which KiCad treats as the board edge; and pads without a hole that
   are listed on both sides were connected on the far side, which KiCad reports as unconnected ("84 of 84 routed"
   was 2 short in KiCad). Both are fixed in the router and in TraceMaker's own DRC.
2. **The DRC is about 100 times faster on boards with large zones** (vme-wren 730 s → 7 s), from the M6 close-out.
3. **From-scratch placement works** where it used to leave parts stacked: 70 % → 85 % clean on boards whose parts
   start in a pile.

## M8: placement from scratch

Details: `docs/04-placement.md` §10. Sets: `bench/place_sets/H.txt` (40 held-out boards, frozen) and S (the same
boards with the movable parts piled at the centre); builder `bench/make_place_sets.py`.

| Run | Clean pass |
|---|---|
| H, human placement | 38 of 40 (39 with the pad fix) |
| S, `full` mode as it was | 28 of 40 (70 %) |
| S, `routable --scratch`, final | **34 of 40 (85 %)**; gate 90 % |

The six failures: three boards have a part that fits nowhere without breaking copper or mask rules that the human
placement itself breaks; kitspace_hack cannot be routed clean in its human placement either; two boards are three
connections short each. So 34 of the 36 boards that can be clean are.

What was built: `tracemaker-place --scratch`; fallbacks that never return to a piled input (pad outline, then
copper only, then beside the board with a message); mask-opening graphics as placement obstacles; `--cut-report`
(R6: no straight line across any of 92 benchmark boards is more than 40 % full). Closed without building: R4, R5,
GPU annealing, CP-SAT windows (reasons in doc 04 §10 and doc 05 §20).

## M11: KiCad GUI, without a desktop

`scripts/kicad_gui_roundtrip.py` runs pcbnew from the KiCad Docker image on an Xvfb display, enables the API, clicks
away the first-run wizard and drives the plugin against the live board:

- tracks 140 → 316 in the open board; one Ctrl+Z removes all of them; Ctrl+Y restores them;
- KiCad's DRC of the board saved from the GUI: unconnected 148 → 65, no new errors;
- with `--place refine`: 65 footprints moved, old copper routed again, zones refilled, two undos restore everything.

The plugin gained `TRACEMAKER_REROUTE`, `TRACEMAKER_PLACE` and `TRACEMAKER_REFILL`. The PCM archive was installed
through the Plugin and Content Manager of the running KiCad (screenshot `report/m11-gui/pcm-installed.png`). Not
checked: that KiCad then builds the plugin's virtual environment and shows its toolbar button (no network for pip
in the container). You may still want to click the button once on your own machine.

## M13, M10, M12, M9, M3

- **M13**: impedance and current rules become net classes (`tmk_<RULE>_<ref>`) that the router uses with
  `--component-rules on`; USB pairs get their own skew limit (doc 15 §15).
- **M10**: `--halving` (successive halving when there are more variants than threads): 12,385 / 12,451 routed at
  2 / 4 threads against 12,402 / 12,508 for simply running the first variants. Off.
- **M12**: `--micro-vias` built (no fixture needs one); see the end of this report for the other three items.
- **M9**: the negotiated fallback for escapes and the template cache are not built; the planner they belong to
  measured below the simple corridors.
- **M3**: the FlatBuffers schema is dropped; the JSON protocol with zstd logs stays.

## Things to look at

- The M8 and M9 gates are not met. Both come down to the same finding as the M6 work: PCBench boards are judged
  under KiCad's default rules, which their own human layouts mostly fail (6 of 92 pass).
- A part that cannot be placed legally is now put beside the board with a message. If you would rather have it
  placed with a reported violation, that is assumption A40.
- `--component-rules on` can make very wide tracks (a 90 Ω pair on a two-layer 1.6 mm board is 0.76 mm wide). It
  is opt-in and unchanged by default.

## Assumptions

`dev/assumptions.md`: A35 (what "complete" means), A36 (the escape census does not count as use of a board), A37
(what set S piles), A38 (`--scratch`), A39 (Margin lines are edges), A40 (unplaceable parts go beside the board),
A41 (hole-less pads have copper on one side), A42 (GPU annealing and CP-SAT closed), A43 (halving restarts).
