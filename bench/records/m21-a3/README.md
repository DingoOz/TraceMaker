# M21 A3 records: the Kookabus boards as a fixture set

Design: doc 18 §4 (A3), §7, §19; decision D176. Branch `m21-kookabus` from `main` 057ee82, 2026-11-08. Judge: KiCad 10.0.6
(`scripts/kicad-cli`, Docker `kicad/kicad:10.0.6`), zones refilled. Machine shared with six other agents' builds and runs; every run is
budgeted by work (`--work 3000000`), so the copper does not depend on the load.

The boards come from the **private** repository `DingoOz/kookabus` (no licence) at 293a922: `scripts/fetch_fixtures.sh kookabus`. No board,
rule file or routed output is committed here; the files below hold counts and measures only.

## Files

| File | What |
|---|---|
| `run.sh` | fetch, prepare (`bench/prepare_demos.py --set kookabus`), the three arms, and the designer's layouts under the same judge |
| `runs/kookabus-<arm>/` | `summary.json` and `boards.jsonl` of `bench/run.py` (base, si, pairs; `-fix` = si and pairs again with D176's critical-net fix) |

## The boards

| Board | Layers | Connections | Custom rules (`.kicad_dru`) | Tuning profiles | Designer's layout (KiCad DRC) | Designer's vias |
|---|--:|--:|---|---|---|--:|
| Kookabus-01 (i2c-battery-board) | 2 | 52 | 2 clearance rules | none | 0 errors, 0 unconnected | 135 |
| Kookabus-02 | 2 | 113 | 1 clearance rule | none | 0 errors, 0 unconnected | 221 |
| Kookabus-03 | 2 | 73 | 2 clearance rules | none | 0 errors, 0 unconnected | 11 |
| Kookabus-04 | 2 | 264 | 1 clearance rule | none | 0 errors, 0 unconnected | 239 |
| Kookabus-04H | 2 | 127 | none | none | 0 errors, 0 unconnected | 146 |
| Kookabus-06 | 4 | 221 | 1 clearance rule | none | 0 errors, 0 unconnected | 396 |

Designer's vias include stitching vias. The rules are same-footprint pad clearances (0.2 mm, one 0.15 mm for a VSSOP-8) and one
`intersectsArea('fine_pitch')` rule; TraceMaker read all of them (no rule reported as not read on any board). Kookabus-05 and -07 have no
layout yet.

## Results (`--work 3000000 --refill`, 6 boards)

Routed / connections, completion after KiCad's refill, vias; no arm added a routing error on any board.

| Board | base (`--soft-zones`) | si (`+ --si-rules`) | pairs (`+ --si-rules --diff-pairs`) |
|---|---|---|---|
| Kookabus-01 | 50/52, 94.2 %, 19 | 50/52, 94.2 %, 19 | 50/52, 94.2 %, 16 |
| Kookabus-02 | 113/113, 97.4 %, 45 | 113/113, 97.4 %, 45 | 113/113, 96.5 %, 43 |
| Kookabus-03 | **73/73, 100 % (clean)**, 15 | **73/73, 100 % (clean)**, 15 | **73/73, 100 % (clean)**, 17 |
| Kookabus-04 | 241/264, 85.6 %, 198 | 238/264, 85.2 %, 202 | 237/264, 84.9 %, 181 |
| Kookabus-04H | 126/127, 97.6 %, 108 | 126/127, 97.6 %, 108 | 125/127, 95.3 %, 105 |
| Kookabus-06 | 204/221, 90.5 %, 136 | 204/221, 91.0 %, 136 | 197/221, 88.2 %, 118 |
| **Clean** | 1/6 | 1/6 | 1/6 |

Kookabus-02 routes every connection but leaves 3 items unconnected after the refill (zone islands); the designer's layout has none.
`--si-rules` changes almost nothing (the boards have little for it); `--diff-pairs` costs completion on three boards.

## What the SI audit finds (`--si-rules`)

| Board | Critical nets | Why |
|---|---|---|
| Kookabus-01 | INA_P/N, SNS_P/N | differential pair (Kelvin current sense) |
| Kookabus-02 | OSC_IN, OSC_OUT | crystal |
| Kookabus-04 | INA_P/N, SNS_P/N | differential pair |
| Kookabus-04H | 4 before D176, **0 after** | the unused D+/D- pins of a charge-only USB-C socket, each a single-pad net (`unconnected-(J1-D+-PadA6)`) |
| Kookabus-06 | SNS1_P/N, SNS2_P/N | differential pair |

Coupled share (`tracemaker pairs`) of those pairs:

| Pair | Designer | si | pairs (`--diff-pairs`) |
|---|--:|--:|--:|
| Kookabus-01 INA | 24.0 % | 0.0 % | 24.9 % |
| Kookabus-01 SNS | 0.0 % | 0.0 % | 0.0 % |
| Kookabus-04 SNS | 0.0 % | 0.0 % | 0.0 % |
| Kookabus-04 INA | 8.6 % | 15.2 % | 0.0 % |
| Kookabus-06 SNS1, SNS2 | 42.6 % | 17.4 % | **95.9 %** |

The designer did not couple these sense pairs tightly either; they are Kelvin connections, where what matters is that the two legs leave the
shunt's own pads, not coupling.

## D176's fix

With the fix, the si and pairs arms were run again (`runs/kookabus-si-fix`, `runs/kookabus-pairs-fix`): every board's routed count, vias
and unconnected count are identical to the runs before it, including Kookabus-04H, whose four critical nets are gone from the audit.
`tests/test_critical.cpp` checks that no critical net has fewer than two pads and that a net reduced to one pad stops being critical. No
plane-tier board has a critical net with fewer than two pads (checked on the eight summaries of `bench/records/m20-run-report`), and
critical nets are computed only with `--critical-auto`, `--critical-nets` or `--si-rules`, so the quick, mid and planes tiers with options
off are unchanged by construction; the quick tier was not run.

## What this says about M21

- B1 (tuning profiles) and B2 (time-domain matching) have nothing to act on: no board defines a tuning profile, a length, skew or delay rule,
  or an impedance class. Doc 18 §8 predicted this. M21's gate (each layer's profile width, `tuning_profile_track_geometries`, delay groups)
  is vacuous on these boards: met trivially, measuring nothing.
- What the Kookabus boards do need is completion: 5 of 6 are not clean at 3 M work, where the designer's layouts all are. That belongs to
  M22 (the last connections) rather than to high speed.
- Not done: more work or variants per board, a placement comparison, and the Kookabus-05 vision board (CSI-2, the one real high-speed
  design), which has no layout yet.
