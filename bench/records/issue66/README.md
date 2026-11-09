# Issue #66: per-layer preferred directions by name, and a 45-degree penalty (D174)

Behind [D174](../../../docs/decisions/D174.md) and doc 05 §73. Branch `issue66-layer-dir` (main `057ee82`), 2026-11-08. Design from
quanln (GitHub @quanln), who asked for both options in the issue and measured the first table row below on 0.12.1.

The board is relaymini (relaymini-c3 from AskAlice/home-automation-pcbs, MIT, tracks removed, zone unfilled), as prepared in quanln's
gist https://gist.github.com/quanln/29e94b4c5d113cc5fbb74d1adf7bba73 (`relaymini.kicad_pcb`, sha256 `7766aae9cd8d3abb...`; not
committed here, as in `bench/records/issue52`). Every run:

```
tracemaker route relaymini.kicad_pcb -o NAME.kicad_pcb --work 3000000 --variants 2 --threads 2 --no-gpu --no-kb --time 3600 [options]
python3 bench/records/issue66/measure_directions.py NAME.kicad_pcb ...        # share of track length per layer
```

`run.sh` runs a list of configurations one after the other; `measure_directions.py` classifies every segment by angle (within 1° of
0°, 90° or 45°/135°, else "other"; arcs are "other") and sums its length per layer. On the default run it gives exactly the issue's
numbers (22 / 30 / 31 / 17 % on F.Cu, 25 / 26 / 42 / 7 % on B.Cu), so the script measures what quanln's did.

## Share of track length (horizontal / vertical / 45° / other, %)

F.Cu is asked to be vertical and B.Cu horizontal (the issue's `--layer-dir-swap` directions).

| Run | F.Cu h / v / 45 / other | B.Cu h / v / 45 / other | Track mm F / B | Routed | Vias |
|---|---|---|---|---|---|
| main, default | 22.0 / 30.1 / 31.4 / 16.5 | 25.0 / 26.4 / 42.2 / 6.3 | 284 / 152 | 60/72 | 44 |
| branch, default | 22.0 / 30.1 / 31.4 / 16.5 | 25.0 / 26.4 / 42.2 / 6.3 | 284 / 152 | 60/72 | 44 |
| main, `--layer-dir-swap --layer-dir-pen 1.0` | 4.3 / 42.5 / 33.0 / 20.2 | 41.4 / 4.4 / 45.7 / 8.5 | 334 / 207 | 61/72 | 51 |
| branch, `--layer-dir-swap --layer-dir-pen 1.0` | 0.6 / 42.0 / 36.8 / 20.6 | 40.9 / 1.4 / 49.6 / 8.1 | 338 / 209 | 61/72 | 51 |
| `--layer-dir F.Cu=v,B.Cu=h --layer-dir-pen 1.0` | 0.6 / 42.0 / 36.8 / 20.6 | 40.9 / 1.4 / 49.6 / 8.1 | 338 / 209 | 61/72 | 51 |
| ... `--layer-diag-pen 0.3` | 0.6 / 38.4 / 38.3 / 22.7 | 48.1 / 1.0 / 43.9 / 7.0 | 321 / 200 | 61/72 | 51 |
| ... `--layer-diag-pen 0.6` | 8.6 / 57.1 / 27.4 / 7.0 | 52.1 / 9.9 / 35.2 / 2.7 | 304 / 228 | 63/72 | 52 |
| ... `--layer-diag-pen 1.0` | 8.1 / 59.1 / 25.5 / 7.2 | 64.8 / 10.3 / 22.9 / 2.0 | 323 / 219 | 62/72 | 54 |
| `--layer-dir F.Cu=v,B.Cu=h` (strength 0.3) | 18.0 / 35.9 / 35.6 / 10.6 | 31.2 / 28.7 / 38.1 / 2.0 | 283 / 235 | 64/72 | 45 |
| ... `--layer-diag-pen 0.3` | 15.3 / 38.8 / 35.3 / 10.6 | 36.1 / 19.8 / 42.0 / 2.1 | 302 / 199 | 64/72 | 46 |

- **Off is byte-identical.** The default output of the branch has the same sha256 as main's (`9183be52a9238a31`). So has a breakout
  board, where D98 turns the parity directions on automatically: srj20 sample001, `--work 3000000 --variants 4` (`a884429bd9d428b2`,
  logs `srj20_sample001_*.log`).
- **Named directions are the parity's directions exactly**: `--layer-dir F.Cu=v,B.Cu=h` gives the same file as `--layer-dir-swap`
  (`b01e8a3a37634e71`).
- **The branch's `--layer-dir-swap --layer-dir-pen 1.0` differs from main's**: with an explicit strength, path smoothing now keeps the
  direction costs (D174), so the last few against-direction runs on F.Cu (4.3 → 0.6 %) and B.Cu (4.4 → 1.4 %) go, and some become
  45° zigzags, which cost less than a pitch per step against the direction. The issue's observation (diagonals go up when the direction
  penalty is on: F.Cu 31 → 37 %, B.Cu 42 → 50 %) holds on the branch as on 0.12.1.
- **The diagonal penalty takes them back**: at 0.6 pitches per 45° step, F.Cu 36.8 → 27.4 % and B.Cu 49.6 → 35.2 %, at 1.0 to
  25.5 % and 22.9 %, below the default's 31 % and 42 %. The "other" share (pad entries off the lattice's directions) falls from
  21 % to 7 % on F.Cu with it. quanln's patch took their PayBox board from 38 % to 23 % diagonal; on relaymini the value that does
  that is about 1.0.
- **Completion** is 60-64 of 72 in every run; one board and one budget cannot tell whether the options help or cost completion (doc 16:
  on the quick tier the parity penalty alone cost 89.2 → 86.8 %). Track length grows by up to 25 % with strength 1.0 (436 → 542 mm).
- `tracemaker drc` finds the same violations in `default`, `dirdiag06` and `dirdiag10` (15 clearance, 12 edge, 4 items not allowed,
  7 shorts: the input board's; KiCad agrees on the edge count in `bench/records/issue52`) apart from unconnected items (26, 22, 22)
  and dangling vias (11, 12, 12).

Files: `run.sh`, `measure_directions.py`, `directions.json` (the table's numbers, per board file), `logs/` (route logs; viewer
tokens removed).
