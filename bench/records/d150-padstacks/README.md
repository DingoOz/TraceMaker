# D150 records: vias and pads that remove unconnected copper, tuning patterns, nearly parallel pairs

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 60, decision D150. Branch `m20-c5-fanout`, 2026-10-28. Judge: KiCad
10.0.6 (`scripts/kicad-cli`, Docker `kicad/kicad:10.0.6`).

## Files

| File | What |
|---|---|
| `vme-wren.json` | KiCad's and the engine's violation counts on vme-wren (KiCad demos) and every diff-pair verdict of both |
| `runs/flash-b1/`, `runs/flash-b2/` | the eight B1/B2 fixtures with pair rules (`bench/records/m21-b2`, `runs/couple-b1`, `runs/couple-b2`) routed by the D150 build |

## Boards that use the option

Four of the fixture boards have vias or pads with `remove_unused_layers`: vme-wren and jetson-agx-thor-baseboard (KiCad demos) and
two srj24 samples (sample009, sample013). None is in a bench tier.

## vme-wren, step by step

`scripts/drc_parity.py` on `bench/data/kicad/demos/vme-wren/vme-wren.kicad_pcb`, against KiCad's report of the same file.

| Build | Clearance | Shorting | Gap errors (KiCad 28) | Uncoupled-length errors (KiCad 6) |
|---|--:|--:|---|---|
| D149 | 22,362 | 21 | DQS0 at 1.8573 mm missing (2) | extra on CK and DQS0–3; DQS0's 6.688 mm missing |
| + flashing | 425 | 20 | the same | DQS3 fixed |
| + tuning patterns | 425 | 20 | the same | the same 6 pairs; DQS0 6.8966 mm against 6.6880 |
| + nearly parallel pairs | 425 | 20 | 28, identical | 6, identical, values included |

KiCad reports no clearance or shorting errors. The engine's remaining 425 and 20 have other causes: pad-to-track pairs judged at
0.125 mm where KiCad resolves 0.1 mm, and shorts against copper graphics. The 34 `length_out_of_range` errors that KiCad reports and
the engine does not are the known gap: `fromTo()` is not evaluated. The engine's DRC takes about 10 minutes on this board, not
investigated.

jetson-agx-thor-baseboard: MATCH on the 15 routing-relevant types with the D150 build.

## Rule parity

234 cases, all matching KiCad 10.0.6. Three are new and the D149 build fails each:

- `via_unused_layers`: four vias, each passed by tracks inside the ring's clearance and outside the hole's.
- `diff_pair_tuning_pattern`: two pairs 0.4 mm apart under a 0.25 mm maximum; the one whose tracks are in a tuning pattern is not
  judged.
- `diff_pair_near_parallel`: a pair 10 nm off parallel over 10 mm is coupled (the lines would cross about 400 m away, beyond 32-bit
  coordinates); a pair 0.1 mm off is not.

## No regression

The eight B1/B2 boards with pair rules, routed again with 3 M work, `--refill --soft-zones`: every output board is byte-identical to
`runs/couple-b1` and `runs/couple-b2` of the D148 build. Their DRC verdicts by the engine are the same for the D149 and D150 builds.
The quick tier is not run: none of its boards has a via or pad that removes copper, a tuning pattern or a pair rule. Full `ctest`
passes: 420 of 420 (1 skipped, `fixture_preparation_native`), `ctest.log`.
