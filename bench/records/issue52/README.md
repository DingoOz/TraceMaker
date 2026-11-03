# Issue #52: custom `hole_to_hole` and `edge_clearance` rules, and `Net` (D168)

Behind [D168](../../../docs/decisions/D168.md) and doc 05 §27 and §33. Branch `hole-edge-rules` (on `pad-number`, main `d7723d6`),
2026-11-03. The board is relaymini (relaymini-c3 from AskAlice/home-automation-pcbs, MIT, tracks removed;
https://gist.github.com/quanln/29e94b4c5d113cc5fbb74d1adf7bba73), the issue's own board not being public, with `rules.kicad_dru`: hole to
hole 1 mm between different nets (`A.Net != B.Net`, the form of WholeBoard's JLCPCB rule) and 0.8 mm from the edge for tracks and vias.

```
tracemaker route r.kicad_pcb -o ro.kicad_pcb --work 3000000 --no-gpu --no-kb --variants 2 --threads 2 --time 3600
kicad-cli pcb drc --severity-all --all-track-errors --format json -o ro_drc.json ro.kicad_pcb     # kicad/kicad:10.0.6
```

| | Routed | Vias | `hole_to_hole` (KiCad) | `copper_edge_clearance` (KiCad) | Other KiCad violations |
|---|---|---|---|---|---|
| Input board | – | – | 0 | 12 | 309 |
| Before (main: `Net` not evaluated, rule dropped; edge rule not read by the router) | 60/72 | 44 | 9 | 18 | 320 |
| After | 59/72 | 35 | **0** | **12** | 320 |

The 12 edge violations left are the input board's own; the other violations (courtyards, silkscreen, library, 11 dangling vias, ...) are
the same before and after. The project sets `hole_to_hole` to warning severity, so `--severity-error` hides them: count with
`--severity-all`. `tracemaker drc` reports the same 9 `hole_to_hole` and 18 / 12 `copper_edge_clearance`.

The after run places 35 vias instead of 44 and routes one connection fewer: the router keeps 1 mm round every new via's hole, a net's
own holes included, where the rule asks it only between nets (D168). With the rule at 0.5 mm, both versions routed 60/72.

The rule-condition corpus (`tests/integration/rule_parity`) gains five `Net` cases judged by KiCad 10.0.6 (`net_code`, `net_code_lone`,
`net_code_zero`, `net_ne_hole_to_hole`, `net_eq_hole_to_hole`); all 250 cases match, main fails four of the five.

Files: `before.log` and `after.log` (route logs), `input_kicad_drc.json`, `before_kicad_drc.json`, `after_kicad_drc.json`, and the rule
file.
