# Throughput after M22 (D158, doc 05 §66)

All runs are 20 M work with `--no-kb`. One-variant runs use `TM_PICK_VARIANTS` (Aleste 5, mini_mass 6, the best variants in the
ten-board base).

## Files

- `searches/`: stderr with one line per search (`TM_DEBUG_CONN` set empty, `TM_DEBUG_TIMING`). The files:
  - `mini_mass-v6-20M`;
  - `Aleste-v5-20M`;
  - `mini_mass-v6-40M-negcap` (`--neg-strict-cap 200000`, 40 M).
- `profile/Aleste-flat.txt`: gprof flat profile (cpu-only build with `-pg -fno-ipa-icf`, one thread). gprof charges clones
  (`.isra`, `.part`) to the symbol before them: `add_long_stub` there is `cell_cost`.
- `fields/one-variant.txt`: one variant, `--gpu-field-min-cells` 0 / 100 k / 250 k / 500 k / 1 M. Each line gives the field count
  and time, the wall time and the md5 of the output.
- `fields/portfolio.txt`: eight variants at 0 and 500 k: each variant's search and field counters, and the result.
- `quick-split/`: the quick tier (30 boards, 1 M work) with the 500 k default. Every output board is byte-identical to `lm-quick-off`.

## Results

| Wall time, 20 M work | 1 variant, 0 | 1 variant, 500 k | 8 variants, 0 | 8 variants, 500 k |
|---|--:|--:|--:|--:|
| Aleste | 27.35 s | 25.51 s | 33.45 s | 29.55 s |
| mini_mass | 65.61 s | 63.68 s | 85.89 s | 78.51 s |

Fields only, one variant:

| Board | all GPU | all CPU |
|---|--:|--:|
| Aleste | 5.8 s | 4.4 s |
| mini_mass | 25.0 s | 71.3 s |

Routed-copper checks on Aleste (a temporary counter, not kept): 11.8 M. Of those, 31 % repeat the same net, cell and width, and 81 %
the same cell and width for any net. Both are upper bounds, since commits in between invalidate some.

## Negotiation on mini_mass (`negotiation/`)

All runs: variant 6, `--no-kb`, every search logged. `N20`, `P20`, `P40` and `P80` also have `--neg-strict-cap 200000
--soft-attempts 1`, as A and B do. `P*` added a trial `--park-capped 1`: a connection whose negotiated searches had run out at the
cap went last in the next pass. It was removed again.

| Run | Work | Options | Routed | Passes | INT nets after pass 1 |
|---|--:|---|--:|--:|--:|
| `A` | 40 M | `--neg-strict-cap 200000 --soft-attempts 1` | 986 | 2 | 11.4 M |
| `B` | 80 M | same | **988** | 3 | 20.4 M |
| `C` | 80 M | same and `--pass-bound` | 986 | 6 | 44.4 M |
| `N20` | 20 M | same as A | 986 | 2 | 0 |
| `P20` / `P40` / `P80` | 20 / 40 / 80 M | same as A and `--park-capped 1` | 986 / 986 / 988 | 2 / 2 / 3 | 0 / 11.4 / 22.0 M |

- **At 20 M** the first pass uses 15.8 M, which leaves 4.3 M for negotiation.
- **At 40 M and 80 M** the three INT nets spend most of the negotiation work on searches that run out at the cap. That happens in
  pass 2, before any requeue, so putting them last changes nothing.
- **The 988 at 80 M** routes other U2 pins than the base: USB_D- and C16 route, +5V U2.24-U2.14 and LCD_HV_nRES fail. The pins
  around U2 lack room, and more negotiation only moves which ones fail.
