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
  copper zones) and, when connections are open, the escape analysis of the stuck report, checked pin by pin with the exact
  proof. On the plane-tier boards this took 0.1 to 2.8 s (doc 18 §9).

## What each section says

| Section | What it shows | Where it comes from |
|---|---|---|
| Top | Routed of all connections; items left unconnected after the zones are refilled (with `--soft-zones`); thermal reliefs the new copper starved (KiCad's `starved_thermal`, which this report's DRC does not check); routing DRC errors added; DRC errors already in the input board; whether the time limit rather than the work budget ended the search (the result then depends on machine speed); rules that could not be read; the next step, from the diagnosis of the open pins | `/routed`, `/connections`, `/open`, `/completion_pct`, `/unconnected_after_refill`, `/refill/starved_thermals`, `/drc/added_routing_errors`, `/drc/before/errors`, `/time_limited`, `/rules/not_read_count`, `/why_open/next` |
| Result | Tracks and vias added, copper layers, the kept result (a portfolio variant, or a rescue pass's), the lattice pitch, the work of the kept result and of the whole run, the wall time | top-level fields of the summary |
| What is open and why | First the diagnosis: an exact proof, past the board's fixed copper only, of whether each open pin can get out at all (walled in, out only with a smaller track or via, walled in only by fills, or open through congestion), with what would help per pin. Then the router's own causes (what failed when it last tried), cut proofs, pins that cannot leave their package (only those the exact proof confirms), the parts with most open ends, each open connection with its last explanation and the nets in the way, and what the board as written leaves unconnected once the zones are refilled, split into nets the router left open and nets the refill cut off | `/why_open`; `/stuck`: the stuck report of doc 17 §3 for the final result, without its advisor parts; `/drc/after/unconnected_items` |
| Relax rescue | When the first pass left connections open: each rescue pass (warm from the first pass's routes, fresh from nothing), how many it routed under the board's rules, whether it was kept, and its relaxed stages (how far the clearance was lowered, and how many routed there before legalising) | `/rescue` |
| Zone refill | With `--soft-zones`: the refill after the route, the refill repair (connections the refill cut off, routed again; kept only if better), the thermal repair, and the starved thermal reliefs by pad | `/refill` |
| Rules | Whether the project and custom-rule files were found, what could not be read (the route may break those, rule 6), board minimums ("not set" when 0) and which governs, net classes with their net counts, custom rules, net classes from the component rules | `/rules`, `/component_rule_net_classes` |
| Signal-integrity audit | Critical nets, differential pairs, buses and return paths, when the options that turn them on were given; it names the parts that were not asked for as not checked | `/critical_nets`, `/pairs`, `/buses`, `/return_paths` |
| DRC before and after | Errors, warnings and unconnected items of the board the router started from and of the board it wrote; per violation type, how many the route added; the input board's own errors with where they are | `/drc` |
| Options and seed | Seed, work budget (of each variant in each pass, not of the run), time limit, variants, threads; every option as given on the command line or in `--config`; the portfolio's variants, pass by pass, with the best of each pass and whether the pass was kept; advisor rounds | `/run`, `/variants`, `/advice` |

Names are shown as KiCad shows them: `{slash}` as `/`, and `~{RESET}` as RESET with an overbar. The JSON keeps KiCad's own spelling.

## Open, cut off, starved

A board with copper zones has three different kinds of "not finished":

- **Open**: a connection the router planned and did not route (`/open`, the stuck table). With `--soft-zones` the router counts a pad
  joined once its track reaches the old fill.
- **Unconnected after the refill**: KiCad refills the zones for the new copper, and a fill the new tracks cut off no longer joins
  what it joined before. The DRC counts these as KiCad does, one per missing link, with its own two ends (so not the router's pads);
  each says whether its net is one the router left open. The refill repair routes the cut-off ones again before the board is written.
- **Starved thermal relief**: the pad is still joined, but the new copper cut its spokes off the pour. KiCad's DRC reports
  `starved_thermal`; TraceMaker's DRC in this report does not, so the report names them from the router's own check.

The DRC's count of unconnected items before the route need not equal the router's connections: the router plans its own (with
`--soft-zones` it does not count the old fills as joining pads).

## Trace a number

In the HTML, every value is underlined with dots: hover over it to see its JSON pointer, or tick **Show the JSON field
of every value** to show all of them, and the source of each table's rows below it. In the Markdown, each value in the
text has a footnote with its pointer, key/value tables have a column of pointers, and other tables name the array
their rows come from (`Rows: /stuck/unrouted/<row>`).

`jq` reads a pointer as a path: `/stuck/unrouted/3/why` is `jq '.stuck.unrouted[3].why' summary.json`.

## What the report does not claim

- The DRC is TraceMaker's own (KiCad's violation names and, for routing rules, KiCad's results, doc 03 §6). It counts
  violations by type, so a type whose count did not grow may still hold a new violation that replaced an old one, and it does not
  check thermal reliefs. Sign off with `kicad-cli pcb drc` ([Run kicad-cli from Docker](kicad-cli-docker.md)).
- Proofs are: a cut proof, a pin the diagnosis proves walled in by fixed copper, and a pin that cannot leave its package (confirmed by
  the same exact proof). "No legal path" and "boxed in" are what the router's search on its lattice found at the time it tried, with
  the copper routed by then; they are not proofs.
- The explanations are the router's own words, written while it worked; they name the last thing that failed, not
  every thing.
