# D152: delay tuning that makes room

Doc 05 §61, decision D152. These are the B2 time-domain fixtures (`bench/records/m21-b2/make_fixtures.py`), routed with 3 M work,
`--refill --soft-zones`, 8 threads, and judged by KiCad 10.0.6.

The `commit` field in each `summary.json` is the commit of the checkout `bench/run.py` ran from. It is not the commit of the binary,
which was set with `TM_BINARY`:

| Run | Binary |
|---|---|
| `runs/main-b2` | main with D151 (fb9981a) |
| `runs/d139-rb` | D139 rebased on main (8131713), before D152 |
| `runs/d152-room`, `runs/d152-b2` | D139 with D152 |

## Results

| Board | main | D139 | D139 + D152 |
|---|---|---|---|
| RoyalBlue54L-Feather-delay | 119/147, none added (`bench/records/d150-padstacks`) | 125/147, `skew_out_of_range` 1 | **125/147, none added** |
| RoyalBlue54L-Feather-profile | 125/147, none | – | 125/147, none |
| interf_u-delay | 145/164, `starved_thermal` 1 | – | 140/164, `starved_thermal` 4 |
| interf_u-profile | 140/164, `starved_thermal` 4 | – | 140/164, `starved_thermal` 4 |

**RoyalBlue54L-Feather-delay.** D+ starts 8.6 ps shorter than D− (70.90 against 79.49 ps):

- *Plain tuner:* reaches 76.80 ps. Its last 0.22 mm meander is blocked by fixed copper.
- *Room step:* starts over from the untuned net, rips one connection and routes it again. D+ ends at 79.43 ps, within the 0.5 ps rule.

The rip-by-proximity version (see the decision's alternatives) changed nothing: the retune reached the same 76.80 ps.

**interf_u.** Neither interf_u board has a delay net that reaches the room step: their tuning logs show no `room for` line. On
interf_u-delay, D139 is the change. Its rules no longer put the router in exact mode, so it routes as interf_u-profile already
routes on main: 140/164 with 4 starved thermals, against 145/164 and 1.

Over the four boards the totals are 529 connections on main and 530 with D139 + D152. Starved thermals go from 5 to 8.

## Tests

- `delay_room` (new): routes RoyalBlue54L-Feather-delay. It passes with 125/147, no skew violation, and one connection ripped and kept.
- `delay_tuning` passes.
- Full `ctest --preset release`: 423 of 424 passed, 1 skipped (`fixture_preparation_native`).
