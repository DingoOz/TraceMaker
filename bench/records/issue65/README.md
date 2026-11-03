# Issue #65: `Pad_Number` and `Pin_Name` in rule conditions

Doc 05 §33. Branch `pad-number` (on main `d7723d6`), 2026-11-03. The board is the reporter's relaymini (relaymini-c3 from
AskAlice/home-automation-pcbs, MIT, tracks removed; https://gist.github.com/quanln/29e94b4c5d113cc5fbb74d1adf7bba73) with
`pad1_keepaway.kicad_dru` (a 0.5 mm clearance to every pad numbered 1):

```
tracemaker route r.kicad_pcb -o ro.kicad_pcb --work 3000000 --no-gpu --no-kb --variants 2 --threads 2 --time 3600
kicad-cli pcb drc --severity-error --format json -o ro_drc.json ro.kicad_pcb     # kicad/kicad:10.0.6
```

| | Routed | `pad1_keepaway` (KiCad) | Other errors (KiCad) |
|---|---|---|---|
| Input board | – | 21 | 58 |
| Before (main `d7723d6`: rule not applied, warned) | 60/72 | 71 | 58 |
| After | 58/72 | **21** | 58 |

The 21 left are the input board's own (pads of different nets already closer than 0.5 mm), and the 58 other errors (24
courtyards, 12 edge, 7 shorts, 7 mask bridges, 4 disallowed items, 4 clearance) are the same on all three boards.
`tracemaker drc` gives the same clearance counts as KiCad with `--all-track-errors` (25 / 82 / 25; KiCad without that option reports
75 on the board before, one violation per track).

The rule-condition corpus (`tests/integration/rule_parity`) gains six cases judged by KiCad 10.0.6: `pad_number`, `pad_number_other`,
`pad_number_ne`, `pin_name`, `pin_name_wildcard`, `pin_name_ne`. All 245 cases match; main fails five of the six. KiCad gives an
NPTH pad an empty number (`PAD::CanHaveNumber`), so `A.Pad_Number != '2'` selects it and `A.Pad_Number == '1'` does not.

Both route runs print "the time limit (3600 s) ended the search before the work budget did" although they take 13 s: the warning is
wrong when the relax rescue's result is kept (a separate bug, not changed here). The results are the same with the default 120 s limit.

Files: `before.log` and `after.log` (route logs), `input_kicad_drc.json`, `before_kicad_drc.json`, `after_kicad_drc.json`, and the rule
file.
