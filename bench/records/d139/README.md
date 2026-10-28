# D139 records: custom rules keep the per-class cache

Board: a WholeBoard design, the 10-port PoE switch (240 x 140 mm, four layers, 1,194 connections), placed by
WholeBoard (`poe_gbe_switch_10p-2-placed.kicad_pcb`; not committed). `wholeboard-rules.kicad_dru` is the rules file
WholeBoard wrote for it. Every run: `tracemaker route --diff-pairs`, 2026-10-27, same machine, other jobs running.

## Eight variants, 300 s

Connections routed per variant (0..7):

| Engine | Rules file | 0 | 1 | 2 | 3 | 4 | 5 | 6 | 7 | Fields | Log |
|---|---|---|---|---|---|---|---|---|---|---|---|
| before D139 | WholeBoard's | 116 | 116 | 221 | 116 | 116 | 1079 | 116 | 92 | 0 | `old-engine-with-rules.log` |
| before D139 | none | 829 | 984 | 1050 | 963 | 820 | 1098 | 990 | 403 | 7,059 GPU | `old-engine-no-rules.log` |
| D139 | WholeBoard's | 116 | 116 | 282 | 116 | 116 | 1100 | 116 | 92 | 1,121 GPU | `new-engine-with-rules.log` |
| D139 | WholeBoard's without the clearance rule | 882 | 1004 | 1057 | 994 | 899 | 1099 | 1006 | 570 | 7,695 GPU | `new-engine-rules-without-clearance.log` |

## Which rule slows the variants (D139 engine, `--variants 1`, 60 s)

| Run | Rules file | Routed |
|---|---|---|
| e1 | none | 58-62 |
| e2 | WholeBoard's | 26 |
| e3 | WholeBoard's without `Copper clearance` | 58 |
| e4 | only `Copper clearance` (0.09 mm, non-zone pairs) | 36 |
| e5 | unconditional clearance 0.15 mm (= the class) | 58 |
| e6 | non-zone clearance 0.15 mm | 58 |
| e7 | unconditional clearance 0.09 mm | 52 |

Evaluating a condition costs nothing measurable (e5, e6). Lowering the clearance below the class's 0.15 mm (to the
0.127 mm board minimum) does, most when zones keep 0.15 mm (e4). Open.

## Quick tier

`quick-compare.md`: 30/30 boards byte-identical against the engine before D139, gates pass.
