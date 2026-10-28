# M21 B2 records: time-domain matching

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 59, decision D147, doc 18 section 17. Branch `m20-c5-fanout`,
2026-10-28. Judge: KiCad 10.0.6 (`scripts/kicad-cli`, Docker `kicad/kicad:10.0.6`). The machine was shared; every run is budgeted
by work (`--work 3000000`, `--refill`, `--soft-zones`).

## Files

| File | What |
|---|---|
| `length_parity.py` | copies boards with a length rule no net can meet, so that both DRCs print every net's length, and pairs the reports |
| `parity/lengths.json` | its output on StickHub, RoyalBlue54L-Feather, interf_u and CM5_MINIMA_3 (as routed by the demos) |
| `make_fixtures.py` | writes `<board>-profile` (a delay profile, no rules) and `<board>-delay` (with rules in ps) for interf_u and RoyalBlue |
| `runs/b2-profile/` | the two `-profile` fixtures routed by the B2 build, to measure the delays the windows were set from |
| `runs/b2-delay-old/` | the `-delay` fixtures routed by the build before B2 (commit 17b2178) |
| `runs/b2-delay-new/` | the same with the B2 build, plus interf_u-profile again with the final class |
| `runs/couple-b1/` | B1's five fixtures (`bench/records/m21-b1`, schema 5) routed by the build that avoids coupling-gap errors (D148) |
| `runs/couple-b2/` | the three B2 boards of `runs/b2-delay-new` routed by that build |
| `timing-verdicts.json` | KiCad's and the engine's `length_out_of_range` and `skew_out_of_range` on the four routed `-delay` boards |

The generated DDR fixture is `bench/make_delay_boards.py` (`ddr8`), checked by `tests/integration/delay_tuning.py`. KiCad's QA board
`time_calculations` is in `tests/boards/time_calculations` and checked by `tests/test_length.cpp`.

## Measurement parity

| Board | Nets in both | Within 0.0001 mm | Within 1 µm | Largest difference |
|---|--:|--:|--:|---|
| StickHub | 45 | 36 | 43 | GND 1.583 mm, +5V 0.043 mm |
| RoyalBlue54L-Feather | 66 | 63 | 64 | /Debugger/D− 0.142 mm, GND 0.040 mm |
| interf_u | 109 | 107 | 109 | /PC-A9 0.0009 mm |
| CM5_MINIMA_3 | 109 | 107 | 109 | +5V 0.0008 mm |

The four large differences are on nets that branch inside a via. There, KiCad 10.0.6 merges chains in the order of the lines'
addresses, and the engine in file order. A simulation of 10.0.6's merge in file order gives the engine's value for /Debugger/D−.

KiCad's QA board: CLASS1–6 delays within 0.005 ps of KiCad's (29.84, 59.84, 59.69, 31.43, 49.84 and 75.72 ps), and the single CLASS7
`skew_out_of_range` that 10.0.6 reports (the reference net measured against itself).

## Fixtures

| Fixture | Class and profile | Rules |
|---|---|---|
| ddr8 (generated, 4 layers) | BYTE0: /DQ0–7, /DM0, /DQS0_P/N; DDR: 5.9 ps/mm outside, 6.8 inside, via 7 ps/mm | window 205–215 ps, skew 3 ps, 0.5 ps within the strobe pair |
| interf_u-delay (2 layers) | MEMDATA: /MD1–4 (complete class); BUS: F.Cu 6.0, B.Cu 6.4 ps/mm, via 7 ps/mm | window 245–260 ps, skew 3 ps |
| RoyalBlue54L-Feather-delay (8 layers) | USB_DIFF (incomplete class, joins Default); USB90: 5.9 ps/mm outside, 6.8 inside, via 7 ps/mm, no geometry | 0.5 ps within each pair |

interf_u's window was set from `runs/b2-profile`, which first used /MD0–7: /MD1–4 took 199–244 ps, /MD0 774 ps, /MD7 346 ps, and
/MD5–6 were unrouted. The class keeps the four that a window can reach, and the window starts above the longest so that every member
has to be lengthened. RoyalBlue's /Debugger pair took 70.90 and 79.49 ps: an 8.6 ps skew against a 0.5 ps rule. The /USB pair is not
routed at this budget.

## Results

| Board | Build | Routed | Window errors | Skew errors |
|---|---|--:|--:|--:|
| ddr8 | before B2 | 17/17 | 11 | 9 |
| ddr8 | B2 | 17/17 | 0 | 0 |
| interf_u-delay | before B2 (17b2178) | 145/164 | 4 | 3 |
| interf_u-delay | B2 | 145/164 | 0 | 0 |
| RoyalBlue54L-Feather-delay | before B2 (17b2178) | 119/147 | – | 1 |
| RoyalBlue54L-Feather-delay | B2 | 119/147 | – | 0 |

The engine's DRC gives KiCad's verdicts on all four routed `-delay` boards (`timing-verdicts.json`). No other KiCad error was added:
interf_u's one `starved_thermal` is in the old run too.

RoyalBlue routes 119 with the rule file and 125 without it (`runs/b2-profile`). On this branch any custom rule other than a disallow
switches the router to exact checks, which turns the distance fields off (0 fields against 185), and the work budget runs out first.
D139, on branch `cache-safe-custom-rules`, keeps the fields for rules like these.

## Diff-pair coupling

The ported coupling check reports the same five `diff_pair_gap_out_of_range` errors on B1's RoyalBlue-profiles as KiCad, with the same
gaps (0.35, 0.35, 0.355, 0.8625 and 1.995 mm against a 0.15 mm maximum) and positions. The rule parity corpus has three new cases:
`diff_pair_gap`, `diff_pair_gap_ignored` and `diff_pair_uncoupled`. All 229 cases match KiCad 10.0.6.

The router now avoids these errors (D148, doc 05 §59): pair nets under a bounded gap rule run the coupling test in the exact check and
on each search step. All runs used 3 M work, `--soft-zones`, and KiCad 10.0.6 as the judge with zones refilled.

| Board | Before (`m21-b1/runs/b1-v5`, `runs/b2-delay-new`) | After (`runs/couple-b1`, `runs/couple-b2`) |
|---|---|---|
| RoyalBlue54L-Feather-profiles | 104/147, 349 tracks, 66 vias, 5 `diff_pair_gap_out_of_range` | 104/147, 353 tracks, 66 vias, none |
| RoyalBlue54L-Feather-base, -se | 125/147, 102/147 | identical |
| plane_smd-base, -profiles | 48/49, 48/49 | identical |
| RoyalBlue54L-Feather-delay | 119/147 | identical |
| interf_u-delay, -profile | 145/164 (1 `starved_thermal`), 140/164 (4) | identical |

"Identical" means the same routed count, tracks, vias and added errors. A first version tested couplings only at commit. It routed
RoyalBlue-profiles 101/147: the search kept finding the USB-C paths that the check then refused.

## No regression

The quick tier (`runs/b2-quick`, 1 M work, 4 threads, the committed B2 binary) is identical to the run before B2 (`m21-quick`, commit
17b2178) on all 30 boards: routed counts, tracks, vias and added errors. Full `ctest` on the B2 build: 415 of 415 passed (1 skipped,
`fixture_preparation_native`).
