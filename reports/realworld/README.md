# Real-world boards study (reports/realworld)

Boards that TraceMaker could not route under their own rules, taken as far as they will go, with every compromise written down and simulated.

## The tools (py/)

| Tool | What it does |
|---|---|
| `closure.py BOARD[+planes] RUN [options]` | The closure loop (EtherCAT shield closed with it, `--bundle-escape --supply-first --no-hard` and seeds, see bench/records/closure/README.md): route (warm start) -> KiCad judge -> remove what stops the routing until it is complete. Remedies, cheapest first (each a transaction, kept only if KiCad then shows fewer open connections): **hard** (an open net with a pour is routed pad to pad, the pour taken out of the router's board and put back for KiCad), **mask** (`--soft-mask-graphics`), **copper graphics** (`--soft-copper-graphics`), **escape plan** (trial of the EtherCAT-review options), **relax** only the open nets and the signal nets crowding them, one tier a round, power nets last (`T0` own rules, `T1` 0.15, `T2` 0.127, `T3` 0.127 with 0.4/0.2 vias), **shove** (nets that just took a tier go one back; the router legalises their copper, cutting long tracks so they bend, `--legalize-subdivide`), **fine finish** (each open net alone on the best copper at a lattice of 10-20 um with its own and its neighbours' rules at the loosest tier), **window reroute**, **part moves** (0.25-1 mm away from an open end point), a 5x-work round; then a **finishing phase** (fine finish and window reroute again, up to three passes). `--view-port N` streams each router to the LAN viewer. |
| `pipeline.py`, `judge_run.sh BOARD RUN LAYERS` | KiCad ledger (`ledger.py`: every violation added by the routing, classified against JLCPCB limits) and the ngspice safety simulation (`safety.py`: crosstalk of every compromised pair, IPC-2221 and DC drop of narrowed copper, via impedance; **assumed** drivers, loads and currents, not datasheet values) |
| `batch.py LIST RUN` / `batch_report.py RUN` | The full stack over many PCBench boards, resumable, results in `batch/RUN.jsonl` and `.md`, shown on the dev page |
| `summary8.py` | The best run of each of the 8 boards under test |
| `prep_board.py NAME LAYERS` | Makes `boards/NAME/` (planes board with the designer's pours, the designer's routing judged as the reference) |
| `crop_plot.py`, `feature_ab.sh`, `shove_try.py`, `tighten_try.py` | Pictures of a region; router-count A/B of the escape features; experiments |
| `test_closure_helpers.py` | Checks of the text helpers (`pcbtext.py`, `rules.py`) |

A board counts as routed when KiCad shows **0 open connections**, the safety simulation is **not FAIL**, and the ledger has **no `shorting_items`** (a track touching copper of another net is a short, not a compromise).
The dev page (`devsite/realworld.py`, port 8770) shows runs live; `bench/records/closure/` holds the results; `dev/morning-report-2026-10-18.md` is the log of the overnight session.

## Router options this study added (all off by default; docs/05-routing.md §45-§52, decisions D124-D131)

`--no-neck-nets`, `--legalize-subdivide`, `--escape-length`, `--escape-persist`, `--radial-supply`, `--via-drops`, `--escape-stubs`, `--soft-mask-graphics`, `--soft-copper-graphics`, `--first-nets-ordered` (§49), `--bundle-escape` / `--bundle-min` (§50: river escapes, the lanes of a package side placed as one block), `--supply-first` (§51), `--package-tunnels` (§52).

Study-tool options of `closure.py` added with them: `--seed`, `--no-power-neck`, `--round0 "ARGS"` (router arguments for the first from-scratch route only), `--region-crossover` (combine step: a donor's nets within 3-10 mm of the open end points), `--package-shuffles/--package-reach/--package-via-drops`, `--no-combine/--no-package/--no-bundle`, `TM_BIN` (a frozen copy of the router for a long run), and the finishing steps `combine`, `package_reroute`, `bundle_reroute`.

`ethercat_qfn_analysis.md` is the owner's review of the EtherCAT shield; `repro/` holds minimal reproductions of router limits.
