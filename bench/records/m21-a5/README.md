# M21 A5 records: #24's critical-net options over five seeds

Design: doc 18 §4 A5 and §16, decision D146. The table of `bench/records/issue24` was one seed. A5 asks for five seeds on the plane
tier before any default changes. Branch `m20-c5-fanout`, 2026-10-28. Binary: `build/scratch/c5x/tracemaker` (C5's crowded-source
portfolio and B3's stitching; snapshotted into each run). `run.sh` runs eight arms × seeds 1-5 on the plane tier (8 boards; 4 have
critical nets, 28 nets by role or pair), with `--work 3000000 --refill --soft-zones`, three runs at a time (assumption A79).

| Arm | Options (with `--soft-zones --seed N`) |
|---|---|
| base | `--critical-auto` (the audit only; routing as without options) |
| first | `--critical-auto --critical-first` |
| split | `--critical-auto --split-cost 4` |
| via | `--critical-auto --critical-via-cost 3` |
| splitvia | `--critical-auto --split-cost 4 --critical-via-cost 3` |
| firstsplit | `--critical-auto --critical-first --split-cost 4` |
| si | `--si-rules --no-si-stitch` (all three) |
| sistitch | `--si-rules` (all three and B3's return stitching) |

## Files

| File | What |
|---|---|
| `run.sh` | the sweep (`$REPO`, `$DEMOS` for the checkout and `bench/prepare_demos.py --set planes` output) |
| `seeds.py` | the tables below from `bench/results/a5-*` |
| `runs/a5-<arm>-s<seed>/` | `boards.jsonl`, `summary.json`, and `critical_nets.json` (each board's critical-net report, stitch and refill summaries) |

## Results

Means over the five seeds, with the range in brackets. Critical nets' vias, void crossings and splits are summed over the routed
critical nets:

| arm | seeds | critical nets routed | their vias | void crossings | splits | unstitched changes | connections routed (all nets) | KiCad unconnected | boards with added errors |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| base | 5 | 24.0 (24–24) | 23.0 (23–23) | 76.0 (76–76) | 22.2 (21–23) | 3.0 (3–3) | 827.2 (822–830) | 80.0 (76–85) | 3.0 (3–3) |
| first | 5 | 27.0 (27–27) | 23.0 (23–23) | 88.0 (88–88) | 27.2 (26–28) | 2.0 (2–2) | 821.2 (816–824) | 85.0 (81–90) | 3.0 (3–3) |
| split | 5 | 25.0 (25–25) | 28.0 (28–28) | 81.0 (81–81) | 23.2 (22–24) | 4.0 (4–4) | 828.2 (823–831) | 80.0 (76–85) | 3.0 (3–3) |
| via | 5 | 24.0 (24–24) | 19.0 (19–19) | 71.0 (71–71) | 21.2 (20–22) | 7.0 (7–7) | 827.2 (822–830) | 77.0 (73–82) | 3.0 (3–3) |
| splitvia | 5 | 23.0 (23–23) | 18.0 (18–18) | 66.0 (66–66) | 21.2 (20–22) | 4.0 (4–4) | 827.2 (822–830) | 79.0 (75–84) | 3.0 (3–3) |
| firstsplit | 5 | 26.0 (26–26) | 24.0 (24–24) | 84.0 (84–84) | 21.2 (20–22) | 6.0 (6–6) | 822.2 (817–825) | 83.0 (79–88) | 3.0 (3–3) |
| si | 5 | 27.0 (27–27) | 25.0 (25–25) | 86.0 (86–86) | 22.2 (21–23) | 1.0 (1–1) | 822.2 (817–825) | 88.0 (84–93) | 3.0 (3–3) |
| sistitch | 5 | 27.0 (27–27) | 25.0 (25–25) | 86.0 (86–86) | 22.2 (21–23) | 1.0 (1–1) | 822.2 (817–825) | 88.0 (84–93) | 3.0 (3–3) |

On the nets both runs routed (`bench/records/issue24/compare_nets.py a5-base-sN a5-<arm>-sN`), seed 1. Seeds 2-5 give the same vias
and crossings; the splits move by 2 together with the base's:

baseline a5-base-s1: 24 of 28 critical nets routed, 830 of 882 connections
| variant | critical nets routed | connections routed | common nets | vias (base -> variant) | void crossings | splits |
|---|---|---|---|---|---|---|
| a5-first-s1 | 27 / 28 | 824 / 882 | 23 | 22 -> 20 | 74 -> 75 | 21 -> 26 |
| a5-split-s1 | 25 / 28 | 831 / 882 | 24 | 23 -> 25 | 76 -> 70 | 23 -> 21 |
| a5-via-s1 | 24 / 28 | 830 / 882 | 23 | 22 -> 18 | 70 -> 67 | 22 -> 19 |
| a5-splitvia-s1 | 23 / 28 | 830 / 882 | 23 | 20 -> 18 | 70 -> 66 | 23 -> 22 |
| a5-firstsplit-s1 | 26 / 28 | 825 / 882 | 23 | 22 -> 22 | 74 -> 75 | 21 -> 22 |
| a5-si-s1 | 27 / 28 | 825 / 882 | 23 | 22 -> 22 | 74 -> 74 | 21 -> 21 |

- **The seed does not move the critical nets.** Every critical-net measure is the same in all five seeds of each arm. Only the routed
  connections of all nets vary, by up to 8 of 882, and KiCad's unconnected count with them. The seed varies only the jittered and
  seeded portfolio variants, and the variant that wins the critical nets is a designed one. The arms' differences are systematic.
- **`--critical-first` routes the nets that matter.** Base and the cost-only arms leave RoyalBlue's crystal net `Net-(U5-XC1)` and its
  debugger USB pair (`/Debugger/D+`, `D-`) unrouted, and one half of a StickHub pair. With critical first only one StickHub half stays
  open (27 of 28). The price is 6 connections of the other nets in every seed (5 with all three rules), and 5 (8) more unconnected
  by KiCad.
- **The cost terms help a little, and less than in #24's table.** On common nets, the via cost cuts their vias by 2-4 and their void
  crossings by 3-4. The split cost cuts crossings by 6 but adds 2 vias. With critical first as well (`si`), the gains vanish (22 → 22
  vias, 74 → 74 crossings). #24's table had splits 20 → 11, on a binary before C5's portfolio change.
- **B3's stitching has nothing to do here.** Under `--si-rules` routing, one layer change of a critical net stays without a return via
  (StickHub, no plane copper on one side) in every seed, with or without the pass. The other arms leave 2 to 7. The `--si-rules`
  routing itself leaves fewer.
- **Added KiCad errors** are the same three boards in every arm and seed: StickHub `solder_mask_bridge`, interf_u `starved_thermal`
  (both also without C5's portfolio, `bench/records/m20-c5/runs/a5x-nocrowd-base-s1`) and multichannel_mixer `starved_thermal` 1 (from
  C5's portfolio; D143).

Per seed:

| arm | seed | critical nets | critical nets routed | their vias | void crossings | splits | unstitched changes | connections routed (all nets) | KiCad unconnected | boards with added errors |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| base | 1 | 28 | 24 | 23 | 76 | 23 | 3 | 830 | 78 | 3 |
| base | 2 | 28 | 24 | 23 | 76 | 23 | 3 | 829 | 78 | 3 |
| base | 3 | 28 | 24 | 23 | 76 | 21 | 3 | 828 | 76 | 3 |
| base | 4 | 28 | 24 | 23 | 76 | 23 | 3 | 827 | 83 | 3 |
| base | 5 | 28 | 24 | 23 | 76 | 21 | 3 | 822 | 85 | 3 |
| first | 1 | 28 | 27 | 23 | 88 | 28 | 2 | 824 | 83 | 3 |
| first | 2 | 28 | 27 | 23 | 88 | 28 | 2 | 823 | 83 | 3 |
| first | 3 | 28 | 27 | 23 | 88 | 26 | 2 | 822 | 81 | 3 |
| first | 4 | 28 | 27 | 23 | 88 | 28 | 2 | 821 | 88 | 3 |
| first | 5 | 28 | 27 | 23 | 88 | 26 | 2 | 816 | 90 | 3 |
| split | 1 | 28 | 25 | 28 | 81 | 24 | 4 | 831 | 78 | 3 |
| split | 2 | 28 | 25 | 28 | 81 | 24 | 4 | 830 | 78 | 3 |
| split | 3 | 28 | 25 | 28 | 81 | 22 | 4 | 829 | 76 | 3 |
| split | 4 | 28 | 25 | 28 | 81 | 24 | 4 | 828 | 83 | 3 |
| split | 5 | 28 | 25 | 28 | 81 | 22 | 4 | 823 | 85 | 3 |
| via | 1 | 28 | 24 | 19 | 71 | 22 | 7 | 830 | 75 | 3 |
| via | 2 | 28 | 24 | 19 | 71 | 22 | 7 | 829 | 75 | 3 |
| via | 3 | 28 | 24 | 19 | 71 | 20 | 7 | 828 | 73 | 3 |
| via | 4 | 28 | 24 | 19 | 71 | 22 | 7 | 827 | 80 | 3 |
| via | 5 | 28 | 24 | 19 | 71 | 20 | 7 | 822 | 82 | 3 |
| splitvia | 1 | 28 | 23 | 18 | 66 | 22 | 4 | 830 | 77 | 3 |
| splitvia | 2 | 28 | 23 | 18 | 66 | 22 | 4 | 829 | 77 | 3 |
| splitvia | 3 | 28 | 23 | 18 | 66 | 20 | 4 | 828 | 75 | 3 |
| splitvia | 4 | 28 | 23 | 18 | 66 | 22 | 4 | 827 | 82 | 3 |
| splitvia | 5 | 28 | 23 | 18 | 66 | 20 | 4 | 822 | 84 | 3 |
| firstsplit | 1 | 28 | 26 | 24 | 84 | 22 | 6 | 825 | 81 | 3 |
| firstsplit | 2 | 28 | 26 | 24 | 84 | 22 | 6 | 824 | 81 | 3 |
| firstsplit | 3 | 28 | 26 | 24 | 84 | 20 | 6 | 823 | 79 | 3 |
| firstsplit | 4 | 28 | 26 | 24 | 84 | 22 | 6 | 822 | 86 | 3 |
| firstsplit | 5 | 28 | 26 | 24 | 84 | 20 | 6 | 817 | 88 | 3 |
| si | 1 | 28 | 27 | 25 | 86 | 23 | 1 | 825 | 86 | 3 |
| si | 2 | 28 | 27 | 25 | 86 | 23 | 1 | 824 | 86 | 3 |
| si | 3 | 28 | 27 | 25 | 86 | 21 | 1 | 823 | 84 | 3 |
| si | 4 | 28 | 27 | 25 | 86 | 23 | 1 | 822 | 91 | 3 |
| si | 5 | 28 | 27 | 25 | 86 | 21 | 1 | 817 | 93 | 3 |
| sistitch | 1 | 28 | 27 | 25 | 86 | 23 | 1 | 825 | 86 | 3 |
| sistitch | 2 | 28 | 27 | 25 | 86 | 23 | 1 | 824 | 86 | 3 |
| sistitch | 3 | 28 | 27 | 25 | 86 | 21 | 1 | 823 | 84 | 3 |
| sistitch | 4 | 28 | 27 | 25 | 86 | 23 | 1 | 822 | 91 | 3 |
| sistitch | 5 | 28 | 27 | 25 | 86 | 21 | 1 | 817 | 93 | 3 |
