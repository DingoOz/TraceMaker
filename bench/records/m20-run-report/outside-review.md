# Outside review of the run reports (D3 gate), 2026-11-08

Reviewer: a separate Claude agent that wrote none of the report code and saw none of the work behind it. It was given only the eight
boards' `report.html`, `report.md`, `summary.json` and `log.txt` (this folder, regenerated on `main` 057ee82 plus the `time_limited` fix of
this branch) and `docs/how-to/run-report.md`, with read access to the source. **This is not a human review**; the D3 gate's "someone who did
not write it" is met only in that sense. The findings and what was done about each are in the README (§4).

---

I reviewed the run reports of eight boards (complex_hierarchy, interf_u, multichannel_mixer, pic_programmer, plane_smd, RoyalBlue54L-Feather,
sonde_xilinx, StickHub): for each, `report.html`, `report.md`, `summary.json` and the console `log.txt`, plus the user page
`HOWTO-run-report.md`. I read every Markdown report in full, checked the main fields of each `summary.json` (counts, refill, DRC, rules, stuck
report, `why_open`, rescue, critical nets) against the report and against the log lines, and compared the HTML and Markdown of each board by
extracting and diffing their number sequences. Where a field's meaning was unclear I read `src/report/render.cpp` and
`src/app/route_job.cpp`, and I looked up the RoyalBlue input board to find which nets U5 pads 12/13 carry.

**Accuracy in short:** every number the reports show equals the JSON field it cites and agrees with the log's `routed …`, `soft zones`,
`refill repair`, `critical nets` and `report:` lines; HTML and Markdown give identical numbers on all eight boards. The errors below are wrong
*statements* (verdicts the report prints that its own JSON/log contradict) and one wrong JSON field.

## 1. Errors (wrong statements or data)

- **E1. "boxed_in" contradicts the run's own analysis, which the report omits.** RoyalBlue (17), pic_programmer (4) and plane_smd (1) are
  `boxed_in`, explained as "Fixed copper (pads, locked tracks, keep-outs) encloses a pad, so no track can leave it". The same runs'
  `/why_open` and log: RoyalBlue "0 walled in by fixed copper …, 37 open through congestion"; pic_programmer "0 walled in by fixed copper …,
  6 open through congestion" (the Why column still says "fixed copper encloses the pin"); plane_smd "0 walled in by fixed copper …, 1 only
  for the class's track or via, … 1 open through congestion". `/why_open` and its `next` advice never appear in the report.
- **E2. RoyalBlue: the escape-proof warning is false.** "Some pins cannot leave their package at all … no amount of routing effort connects
  them" for `U5 | 0.4 | 2 | 12, 13`. U5 pads 12/13 are `/Debugger/D-` and `/Debugger/D+`, both routed in the same report's SI table
  (Unrouted 0, 1 via each).
- **E3. plane_smd: the same warning contradicts the log's advice, which the summary does not carry.** The log says U2.B2 (INT) is walled in
  for the class's 0.1 mm track and 0.45 mm via "but a via of 0.25 mm fits beside it"; `/why_open/next` says "1 pin get out only with a track
  narrower than the net class's" (grammar: "get"). The hint is in the log only; the escape table's "What would help" column is empty on both
  boards that have one.
- **E4. Starved thermal reliefs are missing while the report says 0 DRC errors.** interf_u `/refill/starved_thermals = 4`
  (U3.19, U5.16, U9.C10, U9.K11, U9.L7 before repair), multichannel_mixer `= 1` (C2.2). Neither report mentions thermals.
- **E5. interf_u: "not_attempted" is explained as "The run ended (work or time) before the router tried it again"**, but the run did two
  rescue passes and a refill repair, `/why_open` classes all as congestion, and the Why / In the way columns are empty although
  `/stuck/unrouted/*/crowding/routed_copper_by_net` names the crowding nets. Same for 2 rows on RoyalBlue.
- **E6. interf_u: `/expansions = 0` and every `/work/*` counter 0** for the winning (rescue) result; the log's closing line says
  "0 expansions" too. Not printed by the report, but the summary holds a false value.
- **E7. The SI sentence claims checks that did not run**: "no critical nets, differential pairs, buses or return-path nets were found" when
  only `--si-rules` was given (pairs, buses and return paths were not looked for).

## 2. Misleading or internally inconsistent

- **M1.** The portfolio table repeats variants 0-7 three times (plain, warm rescue, fresh rescue) with no stage column; the JSON has
  `stage`/`rescue_mode`. Discarded rescue rows are indistinguishable from kept ones.
- **M2.** The rescue routes under reduced clearance (ramp stage, -0.04 to -0.10 mm) then legalises; the report never says so.
- **M3.** Each open connection is listed twice (stuck table and DRC unconnected table) with different end pads, and refill cut-offs are mixed
  in; the totals reconcile only by hand. Split "open by the router" from "cut off by the zone refill".
- **M4.** "Unconnected items before" differs from "Connections" with no explanation (RoyalBlue 146 vs 147, plane_smd 47 vs 49).
- **M5.** RoyalBlue's 38 DRC errors already in the input board get no header warning, no locations, and the "Routing: yes" column is unexplained.
- **M6.** Critical-net columns vs warnings: "Void crossings" counts differ from the warnings' "crosses 1 split"; listed detours do not add up
  to the detour total; a detour longer than the net; "runs over 1 void" and "detour round a void" on one row; a length on an unrouted net.
- **M7.** Crystal roles: `/8MH-OUT` and RoyalBlue's XL2 are labelled "crystal xin".
- **M8.** The refill repair (its routed count, kept/discarded, 3-4 M expansions) is invisible; "Work budget per variant 3000000" reads as a
  cap on the run.
- **M9.** Board minimums vs the net class (multichannel_mixer: minimums 0.3/0.3/1.5 mm, Default class 0.2/0.2/0.6 mm): which governs?
- **M10.** "In the way" is empty almost everywhere, even where `/stuck/unrouted/*/crowding` names nets.

## 3. Clarity

Router jargon unexplained ("Search expansions", "Lattice pitch", variant names, stage names in Why strings, "nogood"); KiCad escapes shown raw
(`{slash}`, `~{RESET}`); which end the X/Y of a missing connection is; "Length, mm" (airwire?) and the row order; a board minimum of 0 reads as
an error ("not set"); multichannel_mixer's "joined every connection it planned" followed by 3 unexplained unconnected `vbias` gaps; "See What is
open and why" is not a link. The how-to says the report work takes 0.1-2.1 s (RoyalBlue 3.1 s), calls only cut proofs and escape failures
proofs (see E1/E2), and does not mention rescue passes, the refill repair, starved thermals or Open vs Unconnected after refill.

## 4. Missing for the next decision

The `why_open` diagnosis and advice; the copper layer count; zone health (refill islands, repair outcome, starved thermals); the rescue's relax
hint (StickHub complete at -0.06 mm before legalising); locations of pre-existing DRC errors; that pair coupling and skew were not checked
without `--diff-pairs`; a one-line next step per board.

## What was fine

Every displayed number matches its JSON pointer and the log; HTML and Markdown agree; open counts, per-part open ends and DRC deltas add up;
`time_limited` is false on all eight and the time warning is correctly absent; the rules-not-read sections are correct; the two-verdict
header (router-joined vs after refill) is clear; complex_hierarchy and sonde_xilinx have no problems beyond E7 and the general clarity points.
