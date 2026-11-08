# M20 records: the run report (doc 18 D3, D136)

Results of the tests and runs behind [docs/18-next-features.md](../../../docs/18-next-features.md) section 9, kept in the repository because
`bench/results/` is git-ignored. How to read a report: [docs/how-to/run-report.md](../../../docs/how-to/run-report.md).

- Base: `main` `efdf1d6`, branch `run-report`, 2026-10-24. Machine: 56 cores, shared with other jobs (load 23 to 35 during the runs); the
  GPUs were full, so cost-to-go fields fell back to the CPU.
- Boards: the plane tier (`bench/sets/planes.txt`, prepared by `bench/prepare_demos.py` into `build/demos/`; not committed, as no fixture is).

## 1. Reports for the plane boards (`<board>/`)

`run_reports.sh`: `tracemaker route <board> -o routed.kicad_pcb --work 3000000 --time 3600 --threads 4 --soft-zones --si-rules --no-kb
--view --report report.html --json summary.json`, all eight boards at once, then `tracemaker report summary.json -o report.md` for the
Markdown form. Each folder holds the summary, the report in both forms and the route log. The routed boards are not kept.

Regenerated 2026-11-08 on branch `m20-d3-review` (from `main` 057ee82, with the fixes of §4, D177). Earlier versions of these files (from
2026-10-24 and 10-27) are in git history.

| Board | Routed | Unconnected after refill | Starved thermals | Time-limited | DRC errors before -> after | Routing errors added | Open by the router's cause | why_open: walled in / smaller track or via / congestion | Rescue kept |
|---|--:|--:|--:|:-:|--:|--:|---|:-:|:-:|
| complex_hierarchy | 87/87 | 0 | 0 | no | 0 -> 0 | 0 | - | - | - |
| multichannel_mixer | 173/173 | 3 | 1 | no | 0 -> 0 | 0 | - | - | - |
| pic_programmer | 82/86 | 4 | 0 | no | 0 -> 0 | 0 | boxed_in 4 | 0/0/6 | no |
| plane_smd | 48/49 | 1 | 0 | no | 0 -> 0 | 0 | boxed_in 1 | 0/1/1 | no |
| interf_u | 152/164 | 17 | 4 | no | 0 -> 0 | 0 | not_attempted 12 | 0/0/24 | yes |
| RoyalBlue54L-Feather | 128/147 | 38 | 0 | no | 38 -> 38 | 0 | boxed_in 17, not_attempted 2 | 0/0/37 | no |
| sonde_xilinx | 48/48 | 0 | 0 | no | 0 -> 0 | 0 | - | - | - |
| StickHub | 127/128 | 4 | 0 | no | 0 -> 0 | 0 | ripped_not_rerouted 1 | 0/0/2 | no |

Caveats, honestly:

- RoyalBlue54L-Feather's 38 DRC errors (10 `clearance`, 28 `copper_edge_clearance`) are in the input board; the route adds none. The
  report now warns about them and lists where they are.
- The unconnected count after the refill equals the report's own DRC count of unconnected items on every board: two independent paths
  (the router's refill count and `tracemaker drc` on the written board) agree.
- The report's own work after the route (`report:` line of each log) took 0.1 to 2.8 s.
- An earlier run of these boards with the default 120 s limit routed differently (multichannel_mixer 8 unconnected after refill instead of
  3, StickHub 119 instead of 121 routed): the time limit, not the work, ended those runs. That is why `time_limited` was added (D136).
  On 2026-11-08 a second cause of a false `time_limited` was fixed: after a relaxed last round, the legalising round has a zero time limit by
  design and marked the run time-limited (`src/route/router.cpp`, test in `tests/test_txn.cpp`).

## 2. Every number traceable (`test-report.txt`)

`tests/test_report.cpp`: the HTML of a summary with every section is parsed; each value carries a `data-src` JSON pointer and its text equals
the pointer's field as the report prints it; with the values, code spans, style and title removed, no digit is left in the page (the
renderer's own words carry no number). The Markdown's footnotes all resolve. Also: the rules section's net counts add up to the board's
nets; the DRC comparison counts a track laid over another net's pad as an added routing error; a summary from a plain `--json` run renders
and says what it lacks. Full unit suite: `tm_tests-full.txt`.

## 3. Options off: no change to the copper (`quick-compare.txt`)

`bench/run.py --set quick --work 1000000 --route-args "--variants 1" --jobs 4` with this branch's binary and with `main`'s, same machine:
routed boards byte-identical 30/30, gates pass (clean pass 12/30, mean completion 89.2 % for both; `--variants 1` is a single router, so
this is below the full portfolio's 25/30). Run before the `time_limited` flag was added; the flag is written when the time check fires
and read by nothing in the router, so it cannot change copper.

## 4. The outside review (2026-11-08, D177)

`outside-review.md`: a separate AI agent, which wrote none of the code and saw none of the work behind it, reviewed the eight boards'
reports, summaries and logs and the how-to. **It is not a human review.** Every number matched its JSON field and the log; HTML and
Markdown agreed. Its findings, and what was done about each (decision [D177](../../../docs/decisions/D177.md)):

| Finding | What it said | Done |
|---|---|---|
| E1 | `boxed_in` explained as "fixed copper encloses a pad" while `why_open` says 0 walled in, all congestion; `why_open` and its advice not in the report | **Fixed.** The report opens "What is open and why" with the diagnosis table, the walled-in pins and the neck-down pins; the header prints each `why_open/next` line. `boxed_in` is described as the router's lattice check with the copper routed by then, pointing to the diagnosis. The router's own "fixed copper encloses the pin" (never a proof) now reads "given up: boxed in on the routing lattice, even with routed copper negotiable" |
| E2 | RoyalBlue: U5 12/13 listed as unable to escape, but `/Debugger/D-` and `D+` are routed | **Fixed: the proof was wrong.** The stuck report's package analysis (`route/escape.cpp`) searches a 40 µm lattice; for the USB_DIFF class (0.125 mm track, 0.203 mm clearance, 0.4 mm pitch) it finds no way out, the exact proof (`tracemaker walled`: 0 walled in) does, and the router routed both. Dead pins are now confirmed with the exact proof; the others go to `not_proved` (U5: 12, 13) |
| E3 | plane_smd: the escape warning contradicts the log's "a via of 0.25 mm fits"; the hint is in the log only; "What would help" empty; "1 pin get" | **Fixed.** U2.B2 is no longer listed as trapped (exact proof: out with a smaller via); `why_open/neck_pins` carries the proof's sentence and the report shows it as "What would help"; a still-trapped pin's row says what helps; grammar fixed |
| E4 | Starved thermal reliefs (interf_u 4, multichannel_mixer 1) missing while the report says 0 DRC errors | **Fixed.** Header warning, `refill/starved_thermal_pads` (pad, net, layer, place), a Zone refill section; the DRC section says it does not check thermals |
| E5 | interf_u: `not_attempted` explained as "the run ended"; Why and In the way empty although the crowding names nets | **Fixed.** The legalising round of the relax rescue does not search; what it leaves open now says so (route of the relaxed round broke the clearance, or open in the relaxed rounds too). The cause text names both cases; In the way falls back to the crowding nets |
| E6 | interf_u: `/expansions` 0 and every `/work` counter 0 for the kept rescue result | **Fixed.** A portfolio's work is summed over its rounds (it reported only its last, the legalising round, which does no work). interf_u now 3,507,062 expansions; `work_all_variants` 19.6 M -> 66.5 M (the rescue's relaxed rounds were missing). One-round runs are unchanged |
| E7 | The SI sentence claims pairs, buses and return paths were looked for under `--si-rules` | **Fixed.** The section says what the given options turned on and lists the rest as not checked |
| M1 | Variant table repeats 0-7 three times, no stage | **Fixed.** Pass column (first pass, rescue warm or fresh), best of its pass, pass kept |
| M2 | The rescue's reduced clearance never mentioned | **Fixed.** Relax rescue section: each pass with its relaxed stages (clearance lowered by, routed there) and whether it was kept |
| M3 | Open connections listed twice with different ends; refill cut-offs mixed in | **Fixed** as far as the data allows: each DRC item says whether its net is one the router left open, with both counts (RoyalBlue 19 and 19); the text says the DRC names its own two ends |
| M4 | DRC "unconnected before" differs from Connections | **Explained** in the DRC section (KiCad's count; the router plans its own, and with `--soft-zones` does not count old fills) |
| M5 | RoyalBlue's 38 input-board DRC errors: no warning, no locations, "Routing: yes" unexplained | **Fixed.** Header warning, `drc/before/error_items` with places, column "Routing can cause it" |
| M6 | Critical-net columns vs warnings (void crossings vs splits, detours not adding up, detour longer than the net, length on an unrouted net) | **Fixed** by columns and a note: Splits and Return detour columns, the listing threshold (`critical_nets/detour_listed_over_mm`), "New track, mm" (the new copper of routed connections), "Open connections" |
| M7 | `/8MH-OUT` and RoyalBlue's XL2 labelled "crystal xin" | **Fixed (classifier).** XL2, OSC2 and CLKO name an output pin; named pins where neither says which is the output take the unnamed-pin rule (last pin xout). RoyalBlue: XL2 and XC2 xout. Copper unchanged (both roles are tier 1 and used alike) |
| M8 | Refill repair invisible; "Work budget per variant" reads as a run cap | **Fixed.** Zone refill section with the repair (routed, kept, expansions); budget labelled "of each variant in each pass" with a sentence on what the budget and the time limit bound |
| M9 | Board minimums vs net class: which governs | **Fixed:** the rules section says the router uses the larger of the two per net, and narrows only down to the minimum |
| M10 | In the way empty | **Fixed** with E5 (crowding nets when no blocker is named) |
| Clarity | Jargon; KiCad escapes raw; which end X/Y is; "Length, mm"; 0 minimum; "See What is open and why" not a link; copper layers; how-to timings and missing topics | **Fixed:** names unescaped (`{slash}` as `/`, `~{RESET}` with an overline); "X of From"; "Distance between the pads, mm" in the router's order; "(not set)"; section links; Copper layers and the kept result's expansions in Result; pitch and expansions glossed; how-to updated (sections, open vs cut off vs starved, what is a proof). **Not done:** a glossary of variant and stage names and of "nogood" in the router's Why strings |
| §4 missing | Zone health, relax hint, pre-existing error locations, pairs not checked without `--diff-pairs`, a next step per board | **Done** (Zone refill, Relax rescue, DRC error list, SI not-checked line, "Next:" lines from `why_open`) |

Copper: the routed boards of five plane boards are byte-identical to `main` 057ee82's with the same command (`copper-vs-main.txt`:
plane_smd, pic_programmer, complex_hierarchy, interf_u, RoyalBlue54L-Feather, the last two with the crystal-role change in play), and the
quick tier (`--work 1000000 --route-args "--variants 1" --jobs 4`, main's binary against this branch's) is byte-identical 30/30, gates
pass (`quick-compare-d177.md`).

Is the gate met? The review was made by an AI agent with no part in the code, which meets "someone who did not write it" only in
that sense. It checked the reports against their data, and found real errors, all fixed here. A human designer's read of the fixed
reports, to say whether they tell them what to do next, has not happened, so the M20 D3 gate task is left open (`dev/progress.json`).

