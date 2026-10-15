# 19 — A board session and an agent interface (M18)

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). Written 2026-10-15. Milestone **M18** ([11-roadmap.md](11-roadmap.md)), candidate D2 of
> [18-next-features.md](18-next-features.md). Decision D119.
> Status: stage A (session, tools, two transports) and stage B (evaluation harness, scripted baseline agent) are what this note specifies; §9 says
> what is built and what was measured.

## 1. What it is, and how it differs from the advisor

[Doc 17](17-llm-advisor.md) lets the **engine consult** an advisor: the plain route runs, a stuck report goes out, an advice file comes back, the route
runs again. The agent never sees a board, only a report, and it acts once. Its measured result is that hints derived by a rule gain nothing over the same
compute without hints (doc 17 §8.1), and a model's hints have not been measured.

This document is the other direction: a **session** in which an agent drives the engine. The agent lists the open connections, asks the engine to route some
nets with some hints, looks at what happened, rips a net, goes back to an earlier state, tries something else, and saves. The engine stays what it is: every
copper item passes the exact check, nothing the agent says contains copper or changes a rule, every call is a transaction that is kept or not, and the
whole session replays to the same bytes. PCBWorld (July 2026) reports that agents with engine feedback beat open-loop and grid-action baselines [W, abstract];
whether one can beat TraceMaker's own portfolio at equal compute is the question this milestone answers, with the **stop rule** of §7.

The same session process is the core of the KiCad assistant (M18's second half, candidate D1): a plugin that routes the selected nets without starting the
engine afresh. Only the first half is specified here.

## 2. The rules, applied

| Rule | How the session keeps it |
|---|---|
| 1 Correctness | Copper enters the session only from the router, which checks every segment and via exactly before it commits. `check` runs the DRC on the session's board; `save` reports the errors it added |
| 2 Determinism | A call's result depends on the board, the session's state, the call's arguments and the seed only. Router work is a count (`work`), never a wall time. The log of calls replays to the same board bytes (tested) |
| 3 Reference path | `route` over all nets with the defaults equals `route --variants V` of the command line on the same board and options (tested): the session is a second front door to the same code |
| 4 Transactions | A `route` is kept only if it adds no DRC error (`verify`, default on) and routes at least the connections it set out to improve; otherwise the state is unchanged and the result says why. `undo`, `restore` and `history` go back to any state |
| 5 Budgets | Every `route` takes a `work` budget (search expansions, capped); the session counts the total, and `--work-limit` ends it for good |
| 6 Locked and unreadable | Session copper is locked inside the session: later calls cannot rip it except through `rip_net`, and `rip_net` removes session copper only. The board's own tracks and vias are never removed. Rules that could not be read keep the warnings the engine gives |
| 7 Events | `--view` streams the router's events to the viewer, so a person can watch the agent work |
| 8 KiCad files | `save` re-reads the input file and adds the session's copper through the same editor `route` uses; everything else is byte for byte |

## 3. Safety (the agent is not trusted, and neither is the board)

- **Typed arguments.** Every tool has a schema; an unknown key, a wrong type or a value out of range is an error naming the tool and the key, as for advice files
  (doc 17 §4). `hints` are an advice file, parsed and checked by the same code.
- **No paths from the agent, no commands.** `save` writes to the file named when the session was started (`--out`) and nowhere else. There is no tool that runs
  a program or reads a file.
- **Names are data.** Net names, references and layer names come from the board and may say anything. They appear only as JSON string values, never in the tool
  descriptions or in error templates, and the server's `instructions` tell the agent so (doc 17 §6 wording).
- **Caps.** At most 512 open connections listed per call, 64 nets per `route`, `work` at most 50 M per call, 256 states kept, a request line of at most 1 MB.
- **Redaction.** `--redact` aliases nets and parts (`N1`, `P3`) as the advisor protocol does; the aliases map back before a call is checked, and the saved board has
  the real names.
- **No network.** The server speaks over standard input and output only. A model, a key or a network stays in the agent's own process.

## 4. The tools

Millimetres and the board file's axes; connections are named `REF.PIN`, as in doc 17 (`priority`). Every result is a JSON object; an error is
`{"error": {"code", "message"}}` with the code one of `bad_argument`, `unknown_name`, `limit`, `not_allowed`, `failed`.

| Tool | Arguments | Result |
|---|---|---|
| `board_info` | none | board name, copper layers, counts (parts, pads, nets, tracks and vias of the board and of the session), open connections, state id, work used and left, rules summary (clearance, track width, via) and the warnings from reading the rules |
| `open_connections` | `net` (name, optional), `limit` | the connections still open in the current state: `net`, `a`, `b` (`REF.PIN`), `length_mm`, and, if a route has tried them, `reason`, `why`, `rips`, `fails`, `blockers` |
| `route` | `nets` (names; default: every net with an open connection), `work` (default 2 M, at most 50 M), `variants` (1 to 8, default 1), `seed`, `hints` (an advice object: `first_nets`, `priority`, `layers`, `regions`), `options` (a whitelist: `soft_zones`, `via_in_pad`, `diff_pairs`, `vias_off_pads`, `escape_plan`, `critical_first`), `verify` (default true), `commit` (default true) | `routed_before`, `routed_after`, `connections`, `committed`, `reason` (when not), the new copper per net (tracks, vias, length), `added_errors` (DRC types), the connections still open with their reasons, `work_used`, `state` |
| `rip_net` | `net` | the session copper of that net removed (a new state); how many tracks and vias; refuses a net with none |
| `check` | `full` (default true) | unconnected items and DRC violations by type, and the types and counts the session added relative to the input board |
| `stuck_report` | `redact` | doc 17's report for the last `route` (reasons, crowding, parts at the open ends, `advice_schema`) |
| `history` | none | the states: id, the call that made it, routed connections, vias, copper length |
| `undo` / `restore` | none / `state` | the session goes back one state / to that state (states stay; a new change after a restore starts a new branch) |
| `save` | none | writes the board to `--out`; returns the path, counts and the DRC types added |

A `route` with `commit: false` is a dry run: the result is reported and the state is unchanged.

`hints` apply to this call's run itself, not to extra portfolio variants: in a session the agent decides, sees the result and can undo, so the "advised copies
cannot be worse" guard of doc 17 §4 is not needed; the transaction rule above takes its place.

## 5. Session state and the log

State = the input board, plus a list of session copper (tracks and vias, in commit order), plus the open-connection reasons of the last route. A state is
immutable; each change makes a new one. A router run gets the board with the session copper added and marked locked.

The log is a JSON-lines file (`--log FILE`): per call, the tool, the arguments, a digest of the result and the state's digest (SHA-256 of the session copper in
canonical order). `tracemaker session --replay LOG BOARD --out FILE` runs the calls again and stops at the first digest that differs. Replay is the
determinism test, and it is how a session that went well is reproduced without an agent.

## 6. Transports

One `Session::call(tool, args)` serves both, so they cannot differ.

- **MCP over standard input and output** (`tracemaker session BOARD --out FILE`): JSON-RPC 2.0 with `initialize`, `tools/list`, `tools/call` and the
  `notifications/initialized` notification, protocol revision 2024-11-05 or the client's, `instructions` as in §3. Any MCP client can be the agent (a coding
  agent, an application).
- **JSON lines** (`--jsonl`): one request `{"tool", "args"}` per line, one response per line. For scripts, tests and the evaluation harness.

## 7. The evaluation and its stop rule

**Question.** Does an agent that drives the session route more connections than the plain run at the same compute?

**Compute.** The total of search expansions over all routers of all calls (the summary's `work_all_variants`, the session's `work_used`). The plain run is
`route --work W --variants V`; the agent's session may use at most the same total. The agent's own thinking is not routing work and is not counted, as the
advisor's wall time is not (doc 17 §4); the report says how many calls and tokens it used where the agent can tell.

**Boards.** `bench/sets/srj20-m15fail4.txt`: 16 boards still open after the 70 M run of M15, split into an even half for tuning an agent and an odd half held
out (doc 17 §8.1), so the numbers are comparable with the advisor's. The odd half is looked at once, when the agent is final.

**Arms.** (1) plain, `V` variants; (2) plain with `V + 4` variants (doc 17's control: more compute without hints); (3) the scripted baseline agent of stage B
(§9) at the same total work as (1); (4) a model-driven agent, if one is available, at the same total work.

**Stop rule (decided in advance, D118/D119).** The interface is worth building on only if an agent arm routes **more connections than arm (1) on the held-out
half and is not worse than arm (2)**, summed over the half, on a count that does not move between repeats of arm (1). If neither the scripted nor a
model-driven agent does, the session stays a library (it is still the core of the KiCad assistant) and nothing more is built for agents. A result in
either direction goes into this document with its table.

## 8. What is deliberately not here

- No tool that places copper by coordinates. An agent that wants a route in a place says it through `hints` (a region, a layer cost, an order), and the
  engine finds the copper. A `propose_path` tool (a polyline the router takes as a guide, `RouterOptions::guides`, doc 16 F1) is the next step if §7's result
  is good, and is not built before.
- No placement moves (doc 17 §9, stage 4).
- No model, key or network in the engine (D109).
- No multi-board or multi-user server. One session is one board, one agent.

## 9. Built and measured

*(Filled in as the stages land.)*
