# M18 records: the board session and its first test of the stop rule (doc 19)

Results behind [docs/19-agent-interface.md](../../../docs/19-agent-interface.md) section 9, kept here because `bench/results/` is git-ignored.

- Base: `main` `0099768` plus the roadmap branch; branch `agent-interface`, 2026-10-15. Machine: 56 cores, no GPU used by the router (`--work` budgets: a result does
  not depend on the machine or the thread count). Binary: the release build of the same commit.
- Run: `bench/agent_eval.py --set srj20-m15fail4 --half even|odd --name agent-even|agent-odd --agent "python3 agents/repair_agent.py" --jobs 3`
  (`--work 20000000 --variants 8 --threads 8`, control = 12 variants, agent limit = the control's measured work). The even half was run first to find faults
  in the harness and the agent (see "What went wrong first"); the odd half was run once, with the agent as it was.
- Files: `agent-even.json`, `agent-odd.json` (one row per board: the three arms' routed count, work, errors added, and the agent's calls and rounds; and a summary).

## Result

Connections routed (input board's unconnected items minus the written board's, `tracemaker drc` beside the input's project files). **The plain and control
columns reproduce doc 17 section 8.1 exactly** (430 / 431 on the even half, 439 / 450 on the odd half), which is the check that the arms were run the same way.

Even half (the agent was developed and the harness debugged on it):

| board | open | plain-8 | control-12 | agent | agent work / control work | rounds tried (kept) | calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| sample031 | 65 | 57 | 57 | 57 | 81 M / 97 M | 3 (0) | 25 |
| sample054 | 61 | 56 | 55 | 56 | 73 M / 99 M | 2 (0) | 19 |
| sample094 | 65 | 54 | 56 | 54 | 78 M / 96 M | 4 (0) | 31 |
| sample100 | 52 | 47 | 47 | 47 | 79 M / 98 M | 2 (0) | 19 |
| sample117 | 61 | 53 | 53 | 53 | 70 M / 101 M | 3 (0) | 25 |
| sample119 | 58 | 54 | 55 | 54 | 77 M / 99 M | 2 (0) | 19 |
| sample142 | 69 | 57 | 56 | 57 | 76 M / 104 M | 4 (0) | 31 |
| sample174 | 58 | 52 | 52 | 52 | 80 M / 101 M | 2 (0) | 19 |
| **total** | **489** | **430** | **431** | **430** | **615 M / 794 M** | | |

Odd half (held out, run once):

| board | open | plain-8 | control-12 | agent | agent work / control work | rounds tried (kept) | calls |
|---|---:|---:|---:|---:|---:|---:|---:|
| sample037 | 58 | 51 | 53 | 51 | 69 M / 101 M | 3 (0) | 25 |
| sample056 | 57 | 52 | 54 | 52 | 78 M / 97 M | 2 (0) | 19 |
| sample095 | 65 | 58 | 59 | 58 | 81 M / 98 M | 3 (0) | 25 |
| sample102 | 54 | 52 | 52 | 52 | 73 M / 98 M | 1 (0) | 13 |
| sample118 | 67 | 59 | 57 | 59 | 69 M / 97 M | 3 (0) | 25 |
| sample136 | 62 | 58 | 61 | 58 | 86 M / 106 M | 2 (0) | 19 |
| sample173 | 60 | 49 | 53 | 49 | 67 M / 95 M | 4 (0) | 31 |
| sample191 | 69 | 60 | 61 | 60 | 87 M / 105 M | 3 (0) | 25 |
| **total** | **492** | **439** | **450** | **439** | **611 M / 795 M** | | |

- **Stop rule (doc 19 section 7): not met.** The scripted agent routes exactly what the plain run routes on every one of the 16 boards. Of 43 repair rounds, none
  was kept (each left the board with the same number of open connections or more, and was undone). On the held-out half it is 11 connections behind the
  equal-compute control (439 against 450), the same gap the plain run has: the control's gain comes from the four extra seeds, which a repair of three stuck
  connections does not reproduce.
- It did not spend all it was allowed: 77 % of the control's work (the repair calls are sized to half of what is left, and stop below 500 k per router). The
  unspent 23 % is smaller than the 4 variants the control adds (which cost 47 %), so the comparison is not decided by it, but it is a weakness of this agent.
- The boxed-in pins that these boards leave (every unrouted connection has the reason `boxed_in`) are expensive to attack: a repair call spends mostly flood
  cells (the reachability proof), for example 8.6 M flood cells against 0.85 M expansions for one net.

## Checks that came with it

- **The session's first call is the command line's.** `route --work 20000000 --variants 8 --threads 8` and the session's `route` with the same arguments on
  `srj20` sample054: 56 routed, 596 tracks, 154 vias, 69,573,838 work, both. (Test: `session_cli` compares routed, tracks and vias with `route --variants 1`.)
- **The engine's DRC and KiCad disagree on two boards of this set.** The engine's DRC reports 3 `copper_edge_clearance` errors added by the route on
  `sample031` (plain, control and agent outputs: the track `bga_conn_015` along the bottom edge) and on `sample191` (control output); `kicad-cli` 10.0.6 reports
  none added: sample031 input and output both 65 `copper_edge_clearance`, sample191 input and output both 69. The boards' outline is a rectangle at 9.825 mm
  with a 0.05 mm stroke and the project's `min_copper_edge_clearance` is 0, so the router's tracks lie with their edge exactly on the centre line of the
  outline; the engine's check (`check_edges`, `src/drc/drc.cpp`) takes "touching" as a violation at zero clearance, KiCad does not. This is the reason the session's
  `verify` has the modes `refuse`, `report` and `off`; the agent's first call uses `report`. The DRC difference itself is not fixed here.

## What went wrong first (so the numbers above are read with it)

1. A first version gave each router `left / variants` of the session limit instead of the `work` asked for, so the agent's first call was not the plain run (sample054:
   54 against 56). The limit is now soft and a call runs with the work it asks for.
2. The session's strict verification refused the plain result on `sample031` because of the DRC difference above, so the agent started from nothing (21 routed
   against 57). `verify` became a mode.
3. Written boards were first judged without the input's project files (hundreds of false errors); they are now judged beside copies of them.

## A session driven by hand (illustration, not a measurement)

One even-half board, `sample119` (58 connections; plain 54, control 55), driven by the model that wrote this through the JSON-lines tools (`--work-limit 96000000`):
the plain route (54, four connections open, all `boxed_in`), then rip the first named blocker and route the stuck net first (a different net was left open),
then rip all five named blockers and route six nets with the stuck one first (the stuck net routed, another of the six left open). Six calls later the board still
had four open connections and 17 M of 29 M work was left. What it lacked: the geometry. The tools name nets and pads but say nothing of what lies around a pad, so
the choice of which neighbour to remove is blind; and `stuck_report` after a route restricted to a few nets has no crowding information (the other nets are locked
copper, not "routed copper by net" of that run). A read-only query tool for the neighbourhood of a pad is the obvious next addition.
