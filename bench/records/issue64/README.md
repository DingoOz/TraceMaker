# Issue #64: physical_clearance (D167)

Behind [D167](../../../docs/decisions/D167.md) and doc 05 §27. Branch `physical-clearance` (on main `704a561`), 2026-11-01. The
board is the reporter's relaymini (relaymini-c3 from AskAlice/home-automation-pcbs, MIT, tracks removed;
https://gist.github.com/quanln/29e94b4c5d113cc5fbb74d1adf7bba73) with `via_not_in_pad.kicad_dru`:

```
tracemaker route rB.kicad_pcb -o rBo.kicad_pcb --work 3000000 --no-gpu --no-kb --variants 2 --threads 2
kicad-cli pcb drc --severity-error --format json -o rBo_drc.json rBo.kicad_pcb     # kicad/kicad:10.0.6
```

| | Routed | `via_not_in_pad` (KiCad) | Other errors (KiCad) | `via_not_in_pad` (`tracemaker drc`) |
|---|---|---|---|---|
| Input board | – | 0 | 69 | 0 |
| Before (main `704a561`) | 60/72 | 25 | 69 | 25 |
| After | 57/72 | **0** | 69 | 0 |

The 69 other errors are the input board's own (24 courtyards, 15 clearance, 12 edge, 7 shorts, 7 mask bridges, 4 disallowed
items), the same in all three. `tracemaker drc` does not check courtyards or mask bridges; on the rest it matches KiCad on all
three boards (`clearance` 15 / 40 / 15).

Files: `before.log` and `after.log` (route logs), `before_kicad_drc.json` and `after_kicad_drc.json`, and the rule file.
