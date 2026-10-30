# D159 records: thin tracks, real vias, and the cut test of a routed topology

Results behind [doc 05 §67](../../../docs/05-routing.md) and [D159](../../../docs/decisions/D159.md), kept here because `bench/results/` is git-ignored.

- Base: `origin/main` `33643ee`, branch `thin-vias`, 2026-10-30. Machine: 56 cores, shared with other jobs. No GPU (`--no-gpu`).
- The question: if a board is routed with impossibly thin tracks and the tracks are thickened afterwards, when does the thickening work?
  Relax, route, legalise (D104) is this idea at 60 % of the clearance.

## 1. The cut test (`tracemaker cuts`)

`route/cut_test.{hpp,cpp}`, `tests/test_cut_test.cpp` (`[cuts]`). For every two features (pads, vias) in sight of each other on a copper layer, up to
2 mm apart, the routed centrelines that cross the shortest segment between them must fit in it with their widths and the rule clearances between
neighbours (Cole and Siegel; Leiserson and Maley 1985; Gao, Jerrum, Kaufmann, Mehlhorn and Rülling 1988). Crossings of one wire that enclose no
feature are cancelled in pairs. The via room test moves a via that fails a cut anywhere in its cell. Both are necessary conditions only.

The Python scripts came first and are kept: `tstar.py` (the thickness margin s* of a routed board), `viafit.py` (the via room test, with the
same-net rules), `legal_sweep.py` (the false-positive sweep below).

## 2. No false alarms (`analysis/legal-sweep.tsv`)

`legal_sweep.py tracemaker RUN...` on every board that a benchmark run left KiCad-clean (no error added, nothing unconnected):

| Run | Clean boards | Cuts | Over-full with a wire | Vias without room | Boards with over-full pad-pad cuts (their own spacing) | Smallest s* |
|---|--:|--:|--:|--:|--:|--:|
| full-tierA | 40 | 8,904 | 0 | 0 | 1 | 0.865 |
| full-tierB | 28 | 12,836 | 0 | 0 | 4 | 0.733 |
| full-tierC | 19 | 24,715 | 0 | 0 | 1 | 0.700 |
| full-tierD | 11 | 26,153 | 0 | 0 | 3 | 0.250 |
| full-srj19-rec | 178 | 216,820 | 0 | 0 | 0 | 1.156 |
| full-srj20-rec | 129 | 134,442 | 0 | 0 | 0 | 1.025 |
| **all** | **405** | **423,870** | **0** | **0** | 9 | |

The sweep ran before s* stopped counting cuts between two pads with no wire (the board's own spacing); those are the s* below 1.

## 3. What thickening needs on srj20 (`analysis/vf_*.txt`, `analysis/cuts-*.json`, `analysis/legalised-summary.txt`)

The six srj20 boards that complete at 0.06 mm clearance (`bench/results/diag-c006`), judged at the board's 0.10 mm:

| Board | Wires between pads | Vias without room | Legaliser (D104) |
|---|---|--:|---|
| 071 | all fit | 0 | legal |
| 104 | all fit | 0 | legal |
| 039 | all fit | 0 | projection 2 pairs left, LP legal |
| 103 | all fit | 0 | 3-5 pairs left |
| 048 | all fit | 2 | 4-6 pairs left |
| 072 | all fit | 2 | 9-11 pairs left |

The four vias without room (048: `bga_conn_018` (1.200, 1.860), `bga_conn_055` (-2.910, 4.410); 072: `bga_conn_063` (1.080, 1.200),
`bga_conn_043` (1.200, 0.540)) each sit in a ball-grid cell with a track between the via and a pad: the cell's diagonal is 0.622 mm, a via and a
track need 0.3 + 0.12 + 3 x 0.1 = 0.72 mm. Both legalisers (projection and the LP prototype) leave their violations at exactly those vias.

## 4. Two options on srj20 (`srj20/`)

Boards 048 and 072 (the two with vias without room), `--relax --variants 16 --rounds 25 --time 3600 --threads 8`, one hour each, three arms:
the base, `--ramp-vias-real`, `--ramp-cut-prune`. The runs reached 7-12 rounds in the hour; `srj20/stages.txt` lists every round's relaxed
stage count.

| Board | base | `--ramp-vias-real` | `--ramp-cut-prune` |
|---|--:|--:|--:|
| 048 (74) | 72 | 72 | 72 |
| 072 (81) | 75 | 74 | 75 |

- `--ramp-cut-prune` was identical to the base round by round (no hand-over had a via without room).
- `--ramp-vias-real` routes fewer connections in stage 1 (048: 62-72 against 68-74; 072: 63-73 against 69-76) and finishes no better.
- Boards 103 and 104 were stopped after the first two showed no gain, to free the machine for section 6.

## 5. Where relax lost on PCBench: the legaliser's pads (`probe/`)

The cut test on the relaxed (stage 1) solutions of four PCBench boards, judged at the next stage's rules, found nothing over-full: s\*
1.17-1.76 (ardfpga 1.305, d20 1.195, LFK78 1.764, poncho 1.167). The topology had room, yet the legaliser left 136-354 pairs too close and
dropped a third of the routes at the hand-over.

The cause: the legaliser modelled every pad as an axis-aligned sharp rectangle, a rotated pad as the bounding box of its rotated rectangle.
d20_tri has 526 of 669 pads at 60° or similar and 657 rounded rectangles; poncho has 182 rotated pads and 170 ovals. The boxes overlapped
tracks that were legal, the legaliser pushed them, and the pushes made real violations.

`probe_table.py probe/`: d20 and poncho, `--relax --ramp-cut-prune --variants 4 --rounds 6 --time 3600 --threads 3` (work-bounded rounds of
20 M; the first probe without `--time` was cut by the default 120 s limit and is not used). First hand-over (stage 1, 60 % of the
clearance, to stage 2), and connections routed at the end:

| Arm | d20 pairs before -> after | d20 kept | d20 routed | poncho pairs before -> after | poncho kept | poncho routed |
|---|---|--:|--:|---|--:|--:|
| bounding boxes (`base`) | 1452 -> 136 | 274 of 427 | 326 | 834 -> 354 | 245 of 373 | 278 |
| larger move box 0.5 mm (`box05`) | 1452 -> 164 | 274 | 331 | 834 -> 603 | 243 | 275 |
| subdivide 0.1 mm (`sub01`) | 5394 -> 149 | 280 | 364 | 5713 -> 1313 | 269 | - |
| rotated pads exact (`rot`) | 203 -> 0 | 426 | 350 | 93 -> 0 | 350 | 355 |
| rotated pads exact, subdivide 0.1 (`rotsub`) | 666 -> 0 | 426 | **377** | 1404 -> 0 | 372 | **381** |

(Subdivided pairs count per piece.) `rot` and `rotsub` are rotations only. Ovals and rounded rectangles were added next. With all of
them, a run with `--legalize-subdivide 0.1 --variants 4 --rounds 3` keeps every relaxed route at the first hand-over: d20 0 pairs too
close before, 427 of 427 kept; poncho 42 -> 0 pairs, 379 of 379 kept (`probe/rej-*.txt`).

## 6. The 33 failing PCBench tier B-D boards (`runs/`)

Doc 10 §11.3's set (`bench/sets/pcbench-bd-fail.txt`), `bench/run.py --set pcbench-bd-fail --work 100000000 --jobs 3 --threads 8 --time 3600`,
eight variants, KiCad-judged. The relax arms add `--relax --round-work 20000000 --rounds 5`. Base = `origin/main` 33643ee built as is.
`compare.py` gives the per-board tables: `runs/compare-33.md` (all arms), `runs/compare-subdivide.md` (final against final + subdivide).

| Arm | Engine | Routed | vs base | Better / worse | Clean | CPU s |
|---|---|--:|--:|--:|--:|--:|
| `thin-bd-base8` (no relax) | main | 16,166 | | | 2 | 64,062 |
| `thin-bd-relax-bbox` | bounding-box pads (as main) | 15,683 | -483 | 8 / 16 | 3 | 84,172 |
| `thin-bd-relax-exact` | exact pads | 15,925 | -241 | 8 / 15 | 5 | 82,701 |
| `thin-bd-relax-exsub` | exact pads, `--legalize-subdivide 0.1` | 15,955 | -211 | 10 / 13 | 5 | 91,591 |
| `thin-bd-relax-final9` | this branch | 15,925 | -241 | 8 / 15 | 5 | 60,232 |
| `thin-bd-relax-final9-sub` | this branch, `--legalize-subdivide 0.1` | 15,955 | -211 | 10 / 13 | 5 | 66,714 |

- **Clean boards.** Base: multisensor_cr2032, 58r-mxalps. Relax with exact pads: those two and serial_gw ATMEGA328P, teensy-fx, ardfpga.
  With subdivide: CoreOne instead of ardfpga (212 of 214).
- **Best of base and relax per board.** Base and exact: 16,208 (+42), 5 clean. Base and exact + subdivide: 16,214 (+48), 5 clean.
- **Subdivide 0.1 against none** (both this branch): +30, 12 boards better, 1 worse (ardfpga 214 -> 212), the same five clean boards
  with CoreOne for ardfpga. About 10 % more CPU.
- **The final arms are the same as the earlier ones.** All 33 output boards of `final9` are byte-identical to `exact`, and those of
  `final9-sub` to `exsub`. The in-pad restore below never fired on these boards. CPU seconds depend on the machine's load (the final
  arms ran on a quieter machine), so compare them only within a pair of arms that ran together.
- **No arm added a DRC error.**

**The in-pad restore.** The first version of the restore after the sweeps (a vertex in its own pad that left the pad's copper goes back)
ran in `tm-thin8`. It cost 24 connections on the 21 boards that run had finished, with 0 better and 4 worse (d20 375 -> 356,
OtterPill 126 -> 123, E3 315 -> 314, motor 459 -> 458; `runs/stopped-tm-thin8/`). KiCad's connected count equalled the router's in the
exact arm, which had no restore: no track had come off its pad. The restore now acts only when the end's disc (half the track width)
no longer touches the copper. `runs/replay9/`: d20 375 and OtterPill 126, byte-identical to the exact arm.

**Stopped runs** (`runs/stopped-tm-thin8/`, the strict-restore engine; not comparable as they stand):
- `final` (23 boards) and `final-sub` (16).
- `relax-late` (`--relax --relax-late`, 16 boards). Against exact: +46 (4 better, 4 worse), 2 clean against 4. ErgoDone gained 27 and
  poncho 19; teensy-fx and ardfpga were no longer clean. Worth a full run.

## 7. Subdivide on srj20 (`runs/thin-srj6-*`, `runs/compare-srj6.md`)

Should `--relax` turn on `--legalize-subdivide 0.1` by itself? srj20 is where relax was built, so the six stuck boards were run at the
33-board settings (`--relax --variants 8 --round-work 20000000 --rounds 5`, 100 M work, `runs/srj20-stuck6.txt`), with and without it.
At this budget none completes (the one-hour runs of section 4 reach 72-75).

| Board | relax | relax + subdivide 0.1 |
|---|--:|--:|
| 071 (71) | 66 | 66 |
| 104 (72) | 62 | 61 |
| 048 (74) | 69 | 69 |
| 103 (77) | 70 | 66 |
| 039 (78) | 71 | 72 |
| 072 (81) | 66 | 70 |
| **453** | **404** | **404** |

Neutral (2 better, 2 worse), with 9 % more CPU (13,533 against 14,778 s). Together with +30 on PCBench, subdivide stays off by default.
It is worth adding by hand for boards with many rotated or rounded pads.
