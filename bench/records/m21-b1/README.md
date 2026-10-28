# M21 B1 records: KiCad 10 tuning profiles

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 58, decision D145, doc 18 section 15. Branch `m20-c5-fanout`,
2026-10-28. Judge: KiCad 10.0.6 (`scripts/kicad-cli`, Docker `kicad/kicad:10.0.6`). The machine was shared with other jobs; every run
is budgeted by work (`--work`).

No fixture board defines tuning profiles (doc 18 already noted that the demos' list is empty, and KiCad's own QA project with profiles,
`time_calculations`, sets no widths). `make_fixtures.py` gives two plane-tier boards profiles. Each layer gets a width, and the
project sets `tuning_profile_track_geometries` to `error`, so KiCad checks every profiled track:

| Board | Class | Profile | Outer layers | Inner layers |
|---|---|---|---|---|
| RoyalBlue54L-Feather (8 copper layers) | Default | SE50, single-ended | 0.25 mm | 0.2 mm |
| RoyalBlue54L-Feather | USB_DIFF (the two USB pairs) | USB90, differential | 0.15 mm, gap 0.15 mm | 0.1 mm, gap 0.2 mm |
| plane_smd (4 copper layers) | Default | SE50, single-ended | 0.25 mm | 0.15 mm |

`<board>-base` is the same board and project without profiles; `<board>-se` keeps only the single-ended profile.

## Files

| File | What |
|---|---|
| `make_fixtures.py` | writes the fixtures from `bench/prepare_demos.py`'s output |
| `runs/b1-profiles2/` | all four fixtures, `--work 3000000 --refill --soft-zones`, the build before the exact-check fix (see below) |
| `runs/b1-profiles3/` | the two profile fixtures with the final build |
| `runs/b1-se/` | RoyalBlue with the single-ended profile only, final build |
| `kicad-checks/` | the hand-made board of the unit test with the profile at `error` and at `ignore`, and `kicad-verdicts.json`: KiCad's `track_width`, `clearance` and `diff_pair_gap` verdicts on it, on RoyalBlue as an earlier build routed it, and on the same copper with the pair gap on the net class |

The DRC parity corpus has four new cases (`tests/integration/rule_parity`, frozen KiCad verdicts in `expected.json`):
`static_exact_class_track`, `static_exact_class_case`, `tuning_profile_width` and `tuning_profile_ignored`.

## Results

| Run | Board | Routed | KiCad unconnected | Added KiCad errors | Tracks narrowed | Pairs kept off |
|---|---|--:|--:|---|--:|---|
| `b1-profiles2` | RoyalBlue base | 125/147 | 41 | none | | |
| `b1-profiles3` | RoyalBlue profiles | 120/147 | 46 | none | 68 (to 0.2 mm inside) | In1-In6 |
| `b1-se` | RoyalBlue, single-ended profile only | 125/147 | 41 | none | 68 | |
| `b1-profiles2` | plane_smd base | 48/49 | 1 | none | | |
| `b1-profiles3` | plane_smd profiles | 48/49 | 1 | none | 0 (no inner signal tracks) | |

- **Narrowing is free.** With only the single-ended profile, RoyalBlue routes exactly as without one: the same count in every
  portfolio variant (60, 125, 12, 98, 58, 106, 108, 124). Its 68 inner-layer tracks are 0.2 mm and KiCad finds no width error.
- **The pair profile costs five connections** on RoyalBlue (125 → 120): its pairs are wider (0.15 mm against the class's 0.125 mm) and
  kept to the two outer layers, because the inner layers' 0.1/0.2 mm geometry differs and a pair cannot change gap when it changes
  layer.
- **Bugs found on the way, fixed before the final runs:**
  - The first build compiled the rules only at `error`. KiCad compiles them at `ignore` too, and an ignored one hides the board
    minimum.
  - It evaluated `hasExactNetclass` in user rules, where KiCad 10.0.6 never matches it.
  - It relaxed the clearance between a pair half's track and the other half's pads to the profile's gap. KiCad relaxes only the
    tracks, which gave three `clearance` errors at RoyalBlue's USB connector (`kicad-verdicts.json`).
  - Its profile rules switched the router to exact checks for every rule, which gave up the distance fields (0 fields, against
    182 now). The count was 120 either way. Width and pair-gap rules set no clearance, so they no longer do this.
- `ctest` subset (rule, DRC, parity, pair, report, causes, critical, pour, `plane_smd`): 130 of 130 passed with the build before the
  last fix. Full suite: see the morning report.
