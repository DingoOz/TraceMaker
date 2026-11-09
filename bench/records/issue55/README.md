# Issue #55 records: decoupling caps spread over their IC's supply pins (D171)

Issue #55: `tracemaker-place --scratch` put nearly all decoupling caps of each KSZ9897 (TQFP-128) beside one side of
the chip. D25 ties each cap to the IC supply pad nearest the cap **in the input**. From scratch the input is a pile
(WholeBoard drops the parts to the right of the board), so every cap is tied to the pins that face the pile. D171
adds `--decap-spread` (**off by default**). With it, each cap that has no real position is tied to the least-loaded
supply pad of its net (equal loads taken in bit-reversed pad order). On ICs with 48 or more pads,
`--decap-standoff-mm` (default 2) moves the tie point 2 mm outside the IC's body, so the band next to the pin rows
stays free for escapes.

Variants measured (placement `--mode full --scratch --seed 1`, the engine of this branch):

| Name | Placer options | What it is |
|---|---|---|
| base | none (= main) | nearest pin in the input (D25) |
| spread | `--decap-spread --decap-standoff-mm 0` | spread, tied onto the pin |
| so1 / so2 | `--decap-spread --decap-standoff-mm 1` / `2` | spread, tied 1 / 2 mm outside the IC body (so2 = what `--decap-spread` does) |

The runs were made while the option was still on by default, under the names `new` (= spread) and `so1`/`so2`.
Off is byte-identical to main (checked below).

## The PoE switch (WholeBoard, not committed)

Board: `/home/dingo/WholeBoardProjects-archive/20261027-015953-38a2-i-would-like-an-8-port-ethernet-switch-2-10port/rev5/poe_gbe_switch_10p-1-generated.kicad_pcb`
(rev5: 478 parts, 1,189 connections, 4 layers; rev1-rev6 show the issue, rev7 onwards have the caps locked by WholeBoard). Placed
with `tracemaker-place ... --mode full --scratch --seed 1 --threads 8` plus the options above. Routed with
`tracemaker route ... --diff-pairs --work 10000000 --time 20000 --threads 8 --no-kb` (`poe/route3.sh`;
the rules file is WholeBoard's, so as in D139 only variant 5 gets far).

Caps on a power net whose nearest IC is U10 or U11, by side of the chip (board frame; the chip has 27 supply pins:
L 9, R 7, T 3, B 8; `decap_sides.py`, `poe-decap-sides.txt`):

| Placement | U10 caps L/R/T/B | U11 caps L/R/T/B | median mm to own-net pin | max mm |
|---|---|---|--:|--:|
| WholeBoard's rev5 placement (routable, as in the issue) | 2/23/2/2 | 3/24/1/1 | 3.3 / 3.1 | 6.5 / 6.7 |
| base | 1/25/1/2 | 4/22/1/2 | 2.99 / 2.87 | 6.4 / 5.7 |
| spread | 12/7/5/9 | 9/9/5/10 | 1.70 / 1.70 | 11.5 / 4.5 |
| **so2** | **9/9/4/11** | **9/8/5/11** | 2.99 / 2.99 | **4.9 / 4.9** |

Routing (`poe/*-route.log`):

| Placement | Variant 5 (best plain) | Final routed of 1,189 | Vias | Unrouted pin ends at U10 / U11 |
|---|--:|--:|--:|--:|
| base | 934 | 960 | 521 | 57 / 59 |
| spread | 832 | 869 | 434 | 65 / 67 |
| **so2** | **993** | **993** | 673 | 51 / 68 |

Putting each cap onto its own pin routes 91 fewer connections. With the 2 mm standoff it routes 33 more than base.
`poe-unrouted-sides.txt` gives the open connections at each chip by side. At this budget the open connections are
spread round both chips in all three placements, not only on the cap side. The base result reproduces exactly with
`--time 3600` and with `--time 20000`. The router's "time limit ended the search" warning appears in both, so it is
not what decided the result.

## Public boards (PCBench, piled at the centre)

Eight PCBench boards with a 64-154-pin IC and 10 or more caps on its supplies (`make_piles.py`, which piles every movable part at the
board centre the way set S is built; fixtures `bench/data/issue55-S/`). Run:

    bench/run.py --fixtures bench/data/issue55-S --boards <8 boards> --place full --place-args "--scratch --seed N [opts]" \
                 --work 1000000 --threads 4 --jobs 4 --name i55-pub-<name>

(base with `TM_PLACE_BINARY` set to main's placer). `runs/*/` has each run's `boards.jsonl` and `summary.json`.

All decoupling caps on the eight boards (`summarise.py`, `summary-public.md`):

| Run | decaps | median mm to nearest own-net IC pin | within 5 mm (DEC-01) | busiest-side share at the big IC (mean) | routed of 2,146 |
|---|--:|--:|--:|--:|--:|
| base, seed 1 | 178 | 2.73 | 128 (72 %) | 0.59 | **1581** |
| spread, seed 1 | 178 | 2.52 | 117 (66 %) | 0.51 | 1540 |
| so1, seed 1 | 178 | 2.08 | 131 (74 %) | 0.46 | 1487 |
| so2, seed 1 | 178 | 3.04 | 132 (74 %) | 0.50 | 1549 |
| base, seed 2 | 178 | 2.86 | 129 (72 %) | 0.71 | 1578 |
| spread, seed 2 | 178 | 2.59 | 118 (66 %) | 0.50 | 1518 |
| so2, seed 2 | 178 | 3.05 | 127 (71 %) | 0.47 | **1582** |

Per board: `compare-base-vs-*.md`. Base differs by 3 connections between the two seeds. Spreading the caps onto the
pins costs 41 and 60. With the 2 mm standoff it costs 32 on seed 1 and gains 4 on seed 2, but single boards swing by
up to ±48 (ti_cc3200: 189 -> 141 on seed 1; Own-Mailbox: 110 -> 152). The spread is even round the chip in every
spread run: the busiest side holds about half the caps instead of 0.6-0.7. No board routes clean at this budget in
any variant (piled boards at 1M work).

## Default and identity

Not a clear routing win (PoE +33, public −32 / +4), so `--decap-spread` is off by default. With it off,
`tracemaker-place` output is byte-identical to main (`cmp`). Checked in `--mode full --seed 1 --threads 4` on the
ice40 pile (`--scratch`), the ice40 human board, and the medusa_cape pile (`--scratch`). The quick tier and set S
were not run: the default path is unchanged.

## Files

- `decap_sides.py` caps per side of an IC; `unrouted_sides.py` open connections per side; `compare_runs.py`,
  `summarise.py` the tables above; `make_piles.py` the public piles.
- `poe/` the placer and route logs of the PoE board (the live-view token lines removed).
