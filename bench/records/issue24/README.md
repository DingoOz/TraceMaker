# Issue #24 records: JLCPCB's three routing rules

Design: [docs/05-routing.md](../../../docs/05-routing.md) section 44, decision D117, doc 15 (`reference_plane` row), protocol note in doc 13 (`critical_nets`), viewer panel in doc 09.
The rules (JLCPCB, 10 Oct 2026): route critical signals first; minimise vias; keep a solid reference plane and never cross a split. All three are **options, off by default**.

## What was built

| Rule | Option | How |
|---|---|---|
| critical signals first | `--critical-nets PATTERNS`, `--critical-auto`, `--critical-first` | nets named, or found from the component rules' roles (`crules::critical_roles()`) and from differential pairs, in three tiers; routed first tier by tier, after `--first-nets`, also after restarts |
| fewer vias | `--critical-via-cost F`, `--critical-via-max N` | a via costs a critical net F times as much in every search; a via count per critical net in the report, a warning over N |
| never cross a split | `--split-cost P`, `--split-hard` | a lattice map of the cells over a void of the adjacent reference plane (`route/ref_map.hpp`, from the zone fills); critical nets pay P pitches per step there, or may not enter; the audit of doc 05 §42 reports what is left |
| all three | `--si-rules` | critical-auto + critical-first + via x3 + split cost 4 (a value given beside it wins) |
| report | log, `--json` summary `critical_nets`, `critical_nets` and `return_path` events, viewer panel | per critical net: tier, why, vias, length, unrouted, void crossings, splits, warnings |

Found on the way (both have tests): the router hopped onto the plane layer to cross a slot where nothing was charged, and the smoothing pass shortcut a critical net's detour back over the split. A first design
that gave critical nets bigger search windows from the start made `--si-rules` route 3 of 147 connections on RoyalBlue (the work budget went into failing critical connections' big windows): it now searches a
wider window only after a critical net's first path crossed a void or hopped layers (`refine_critical`).

## Review

An independent read of the first version (a fresh agent, read-only) found no change of behaviour with the options off, and these defects, fixed in the second commit with tests: `--split-hard` could close a through-hole
critical pad in (the plane's antipad round it counted as a void: cells within the pad's radius + 1 mm are now exempt); a refined path that failed the exact check lost the legal first path (the first is now committed as it
was); `--critical-auto` ignored `--rules-override`; the in-pad via price was not scaled by the via multiplier; the smoothing pass summed void samples cubically (prefix sums now); every portfolio variant refilled the
board for the map (once, now, in the route job). Docs and help text that described an earlier design (windows two sizes up; named nets ahead of tier 1) were corrected.

## What was checked (`verification/`)

- `ctest-release.txt`: the full suite; new: 8 cases in `tests/test_critical.cpp` (the rasterised plane equals a point-in-region scan cell by cell; the map finds planes and voids; a critical net goes round a split
  when there is a way and only a critical one; with no way round it crosses and the audit says so, or stays unrouted under `--split-hard`; a board with no plane routes as before, byte for byte; critical order by
  tier, with `--first-nets` in front; a dearer via takes the longer way on one layer; roles make nets critical) and `critical_nets_cli` (options on the `tui` screen and in `--help`, a scripted run of the screen, the
  summary and log on a board with a split plane, `--si-rules`, `--split-hard`, report-only runs change no copper).
- `quick-compare.txt`, `planes-compare.txt`, `planes-soft-compare.txt`, `mid-compare.txt`: options off: boards byte-identical to `origin/main` (30/30, 8/8, 8/8, 11/11; CPU seconds are higher than the baseline's because other jobs shared the machine). `report-only-identical-*.txt`: `--critical-auto` alone (a report, no cost, no order) is byte-identical on the
  plane tiers too (8/8, 8/8): the audit does not touch the copper.
- `sanitizers.txt`: the critical tests and the CLI test under ASan + UBSan.
- `e2e/`: headless Chromium against a live `route --si-rules --view --hold` on RoyalBlue54L-Feather (`e2e-critical-nets.mjs`): the Critical nets panel, a click that zooms onto a net and shows its return path.

## What the rules did on the plane tiers (`critical-nets-fair-*.txt`, `compare-*.txt`)

Planes tier (8 boards; 4 of them have critical nets: 28 nets by role or pair), 3M work units per board, the same board set as the options-off runs. The baseline is `--critical-auto` alone: the unchanged copper with the audit on it
(byte-identical to the options-off run, `report-only-identical-*.txt`). Compared by `compare_nets.py` on the nets both runs routed fully, so that a net one run could not route does not count as crossing nothing. Crossings are voids of the
reference plane under the net, via antipads included. All runs on the final binary.

With `--soft-zones` (the realistic mode: plane copper is routable), baseline 25 of 28 critical nets routed, 807 of 882 connections, mean completion 91.3 %:

| options | critical nets routed | connections routed | common nets | vias | void crossings | splits | mean completion |
|---|---|---|---|---|---|---|---|
| `--critical-first` | 26 | 806 | 23 | 18 -> 20 | 62 -> 72 | 22 -> 23 | 91.4 % |
| `--split-cost 4` | 21 | 805 | 21 | 18 -> 16 | 57 -> 52 | 20 -> 11 | 91.5 % |
| `--critical-via-cost 3` | 21 | 805 | 21 | 18 -> 12 | 57 -> 47 | 20 -> 11 | 91.1 % |
| `--split-cost 4 --critical-via-cost 3` | 21 | 805 | 21 | 18 -> 14 | 57 -> 48 | 20 -> 11 | 91.1 % |
| `--critical-first --split-cost 4` | 25 | 801 | 22 | 17 -> 17 | 63 -> 68 | 21 -> 18 | 91.2 % |
| `--si-rules` (all three) | 27 | 799 | 24 | 19 -> 15 | 70 -> 68 | 26 -> 25 | 90.5 % |

Without `--soft-zones` (plane fills block routing, completion 56.5 % either way): the baseline routes 3 of the 28 critical nets and `--si-rules` 4, with the same 396 of 879 connections.

What this says, and does not: the split cost and the via cost, without the order, roughly halve the splits crossed (20 -> 11) and cut vias (18 -> 12 to 16) on the nets both routed. `--critical-first` on its own adds crossings and cancels most of the split cost's
benefit when it is added, so `--si-rules` (all three, as the issue lists them) is not the best of these: `--critical-auto --split-cost 4 --critical-via-cost 3` is. That is one seed on four boards, and noise is of the same size as some of
the differences: the critical nets that the split-only and via-only runs fail to route (four USB pairs on StickHub) are also unrouted in a single-variant baseline run, so which portfolio variant wins moves the routed counts by a few nets and a
few connections. The clean-pass count (2 of 8) did not change in any run. An earlier version of this table counted unrouted critical nets as routed (the per-net `unrouted` field did not exist yet) and said `--si-rules` routed all 28: it did not;
`aggregate.py` and `compare_nets.py` now refuse a summary without the field.

## Not covered

- Defaults stay off: the effect is measured on four boards and one seed, `--critical-first` did not help the crossings, and the values in `--si-rules` (x3, +4 pitches per step) are the first ones tried, not tuned.
- The map knows the fills of the file, not the antipads of vias the route adds; only the smoothing pass is split-aware (not length tuning or pair coupling); a rule's own `plane_net` is not read; a plane under 30 % of the board is not a reference.
- No per-role via budget or hard cap (the report threshold is the cap). Net classes from the roles are not made.
- Nobody has used these on a board of their own, and the viewer panel's look was judged on screenshots only.
