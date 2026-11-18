# sample152 at 4 copper layers (2026-11-18)

sample152 (srj20) has never routed clean on 2 layers: the best of the campaign leaves 2 open (bga_conn_055, 061) after
1 h runs, ramps and rip-ups of 4 to 191 nets (`../fix055`). This run asks whether 4 layers would close it, with the
existing `route --copper-layers 4` (doc 05 §53); `route --try-layers 2,4` runs the same 4-layer route only when 2 layers
fail.

```
tracemaker route .../srj20/sample152/unrouted.kicad_pcb -o out.kicad_pcb --json out.json --copper-layers 4 --relax \
  --variants 16 --threads 8 --fast-bends --no-kb --time 900 --time-total 900 --seed 121 --view --view-port 8931
```

| | 2 layers, best of the campaign | 4 layers, through vias |
|---|--:|--:|
| Routed | 85/87 | **87/87** (all 16 variants) |
| Best variant | 1 h + ramp | 22.3 s |
| Vias | | 153 |
| Tracks per layer | | F.Cu 364, In1 100, B.Cu 95, In2 87 |
| KiCad unconnected | 2 | 0 |
| KiCad violations | 341 | 341 |

The 341 violations are the input's own (overlapping test points: 83 shorts, 83 mask bridges, 87 edge clearances, 88
library issues) and appear on the unrouted board too; routing adds none. Blind vias were not needed. The routed board
is not kept here (srj20 boards carry no licence).
