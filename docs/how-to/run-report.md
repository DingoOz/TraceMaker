---
status: current
code: [src/report, src/app/route_job.cpp]
---

# Read a run report

A run report is one file that tells you what a `route` run did: what it routed, what is still open and why, which rules
it read and which it could not, the signal-integrity audit, the DRC of the board before and after, and the options and
seed that repeat the run (doc 18 D3, decision [D136](../decisions/D136.md)).

## Make one

```
tracemaker route board.kicad_pcb -o routed.kicad_pcb --report report.html
```

- `--report FILE` writes HTML, or Markdown when `FILE` ends in `.md`.
- The report is rendered from the run's JSON summary and nothing else. With `--json SUMMARY` the summary goes there;
  without it, it is written beside the report as `FILE.json`. Keep the two together: the report's numbers are fields of
  that file.
- `tracemaker report SUMMARY.json -o FILE` renders the report again from a saved summary. A summary written without
  `--report` has no rules, stuck report or DRC sections, and the report says so.
- `--report` adds work after the route: a DRC of the board before and after (zones refilled when the board has
  copper zones) and, when connections are open, the escape analysis of the stuck report. On the plane-tier boards this
  took 0.1 to 2.1 s (doc 18 §9).

## What each section says

| Section | What it shows | Where it comes from |
|---|---|---|
| Top | Routed of all connections, items left unconnected after the zones are refilled (with `--soft-zones`), routing DRC errors added, whether the time limit rather than the work budget ended the search (the result then depends on machine speed), rules that could not be read | `/routed`, `/connections`, `/open`, `/completion_pct`, `/unconnected_after_refill`, `/drc/added_routing_errors`, `/time_limited`, `/rules/not_read_count` |
| Result | Tracks and vias added, the winning variant, the lattice pitch, the work done, the wall time | top-level fields of the summary |
| What is open and why | Open connections by cause, with what each cause means; cut proofs (lines more nets must cross than tracks fit through); pins that cannot leave their package; the parts with most open ends; each open connection with the router's last explanation and the nets in the way; what the board as written leaves unconnected once the zones are refilled | `/stuck`: the stuck report of doc 17 §3 for the final result, without its advisor parts; `/drc/after/unconnected_items` |
| Rules | Whether the project and custom-rule files were found, what could not be read (the route may break those, rule 6), board minimums, net classes with their net counts, custom rules, net classes from the component rules | `/rules`, `/component_rule_net_classes` |
| Signal-integrity audit | Critical nets, differential pairs, buses and return paths, when the options that turn them on were given | `/critical_nets`, `/pairs`, `/buses`, `/return_paths` |
| DRC before and after | Errors, warnings and unconnected items of the board the router started from and of the board it wrote; per violation type, how many the route added | `/drc` |
| Options and seed | Seed, work budget, time limit, variants, threads; every option as given on the command line or in `--config`; the portfolio's variants; advisor rounds | `/run`, `/variants`, `/advice` |

## Trace a number

In the HTML, every value is underlined with dots: hover over it to see its JSON pointer, or tick **Show the JSON field
of every value** to show all of them, and the source of each table's rows below it. In the Markdown, each value in the
text has a footnote with its pointer, key/value tables have a column of pointers, and other tables name the array
their rows come from (`Rows: /stuck/unrouted/<row>`).

`jq` reads a pointer as a path: `/stuck/unrouted/3/why` is `jq '.stuck.unrouted[3].why' summary.json`.

## What the report does not claim

- The DRC is TraceMaker's own (KiCad's violation names and, for routing rules, KiCad's results, doc 03 §6). It counts
  violations by type, so a type whose count did not grow may still hold a new violation that replaced an old one. Sign
  off with `kicad-cli pcb drc` ([Run kicad-cli from Docker](kicad-cli-docker.md)).
- "No legal path" means the search found none, not that none exists. Only a cut proof or a pin that cannot escape its
  package is a proof.
- The explanations are the router's own words, written while it worked; they name the last thing that failed, not
  every thing.
