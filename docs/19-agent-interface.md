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
| 5 Budgets | Every `route` takes a `work` budget in search steps (expansions plus flood cells). The router's budget stops new searches from starting; the search in progress and the clean-up after the last one finish, so a call can pass it. The session counts what was really spent (`work_used`) and refuses the next call when `--work-limit` is spent. The limit is soft, and a call runs with the `work` it asks for (so a first call is the command line's): an agent that must stay under the limit sizes `work` from `board_info`'s `work_left` |
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
- **No redaction in the session.** An agent that drives it sees the board's names. Where names must not leave the machine, the advisor protocol's `--advisor-redact`
  (doc 17 §3) is the tool; a redacted session would need aliases in every result and is not built.
- **No network.** The server speaks over standard input and output only. A model, a key or a network stays in the agent's own process.

## 4. The tools

Millimetres and the board file's axes; connections are named `REF.PIN`, as in doc 17 (`priority`). Every result is a JSON object; an error is
`{"error": {"code", "message"}}` with the code one of `bad_argument`, `unknown_name`, `limit`, `not_allowed`, `failed`.

| Tool | Arguments | Result |
|---|---|---|
| `board_info` | none | board name, copper layers, counts (parts, pads, nets, tracks and vias of the board and of the session), open connections, state id, work used and left, rules summary (clearance, track width, via) and the warnings from reading the rules |
| `open_connections` | `net` (name, optional), `limit`, `soft_zones` | the connections still open in the current state: `net`, `a`, `b` (`REF.PIN`, or `zone` for a plane), `length_mm` (not for a plane), and, if a route has tried them, `reason`, `why`, `rips`, `fails`, `blockers`. `soft_zones` plans the plane connections too (default: on once a route with soft zones was kept) |
| `route` | `nets` (names; default: every net with an open connection), `work` (default 2 M, at most 50 M), `variants` (1 to 8, default 1), `seed`, `hints` (an advice object: `first_nets`, `priority`, `layers`, `regions`), `options` (a whitelist of booleans: `soft_zones`, `via_in_pad`, `diff_pairs`, `vias_off_pads`, `escape_plan`), `verify` (default true), `commit` (default true) | `routed_before`, `routed_after`, `connections`, `committed`, `reason` (when not), the new copper per net (tracks, vias, length), `added_errors` (DRC types), the connections still open with their reasons, `work_used`, `state` |
| `rip_net` | `net` | the session copper of that net removed (a new state); how many tracks and vias; refuses a net with none |
| `check` | `full` (default true), `refill` | unconnected items and DRC errors by type, and the types and counts the session added relative to the input board. `refill` judges with the zones refilled, as `kicad-cli --refill-zones` does (default: on once a route with soft zones was kept; a route with soft zones is verified refilled, because the router counts on the refill) |
| `stuck_report` | `redact` | doc 17's report for the last `route` (reasons, crowding, parts at the open ends, `advice_schema`) |
| `history` | none | the states: id, the call that made it, routed connections, vias, copper length |
| `undo` / `restore` | none / `state` | the session goes back one state / to that state (states stay; a new change after a restore starts a new branch) |
| `save` | `refill` | writes the board to `--out` (the result does not name the path: a replay may write elsewhere); returns counts and the DRC types added |

A `route` with `commit: false` is a dry run: the result is reported and the state is unchanged.

`hints` apply to this call's run itself, not to extra portfolio variants: in a session the agent decides, sees the result and can undo, so the "advised copies
cannot be worse" guard of doc 17 §4 is not needed; the transaction rule above takes its place.

## 5. Session state and the log

State = the input board, plus a list of session copper (tracks and vias, in commit order), plus the open-connection reasons of the last route. A state is
immutable; each change makes a new one. A router run gets the board with the session copper added and marked locked. The first listing of the open
connections of a state costs one router set-up (the planner needs the obstacle model); it is cached per state. A long-running daemon that keeps the set-up
between calls is the other half of M18 and is not built.

The log is a JSON-lines file (`--log FILE`): per call, the tool, the arguments, a digest of the result and the state's digest (FNV-1a 64 of the session copper
in canonical order: a digest to compare a session with its replay, not a cryptographic hash). `tracemaker session --replay LOG BOARD --out FILE` runs the calls again and stops at the first digest that differs. Replay is the
determinism test, and it is how a session that went well is reproduced without an agent.

## 6. Transports

One `Session::call(tool, args)` serves both, so they cannot differ.

- **MCP over standard input and output** (`tracemaker session BOARD --out FILE`): JSON-RPC 2.0 with `initialize`, `tools/list`, `tools/call` and the
  `notifications/initialized` notification, protocol revision 2024-11-05 or the client's, `instructions` as in §3. Any MCP client can be the agent (a coding
  agent, an application).
- **JSON lines** (`--jsonl`): one request `{"tool", "args"}` per line, one response per line. For scripts, tests and the evaluation harness.

## 7. The evaluation and its stop rule

**Question.** Does an agent that drives the session route more connections than the same compute spent without an agent?

**Compute.** Search expansions plus flood cells over all routers of all calls: the summary's `work_all_variants.total` for a `route` run, `work_used` for a
session. The agent's own thinking is not routing work and is not counted, as the advisor's wall time is not (doc 17 §4).

**Arms** (`bench/agent_eval.py`), each scored the same way: connections routed = unconnected items of the input board minus those of the written board, judged
by `tracemaker drc` beside the input's project files; an arm that adds a DRC error is invalid for that board.

1. **plain**: `route --work W --variants V` (W = 20 M, V = 8: doc 17 §8.1's plain arm).
2. **control**: `route --work W --variants V+4`. More compute with no agent, and **the equal-compute reference**: its measured work is the agent's `--work-limit`.
3. **agent**: a program that drives `tracemaker session` with that limit. Its first call is the plain arm (so it cannot do worse than arm 1 by construction); the
   rest of the compute is the agent's to spend.

**Boards.** `bench/sets/srj20-m15fail4.txt` (16 `srj20` boards still open after the 70 M run of M15, doc 17 §8.1): an even half to tune an agent on and an odd
half held out, looked at once when the agent is final. The same boards and arms as doc 17 §8.1, so the plain and control columns must reproduce its totals
(430 and 431 on the even half, 439 and 450 on the odd half); if they do not, the arms were not run the same way and nothing else is read.

**Stop rule (decided in advance, D118/D119).** The interface is worth building on for agents only if an agent arm routes **more connections than the control
(arm 2) on the held-out half**, summed over the half and not worse on most boards. If neither the scripted agent of stage B nor a model-driven one does,
the session stays a library (it is still the core of the KiCad assistant) and nothing more is built for agents. The scripted agent is a baseline, not the
test of a model: a negative result for it says that these rules do not beat seeds, not that a model cannot.

## 8. What is deliberately not here

- No tool that places copper by coordinates. An agent that wants a route in a place says it through `hints` (a region, a layer cost, an order), and the
  engine finds the copper. A `propose_path` tool (a polyline the router takes as a guide, `RouterOptions::guides`, doc 16 F1) is the next step if §7's result
  is good, and is not built before.
- No placement moves (doc 17 §9, stage 4).
- No `critical_first`, `relax` or other portfolio options among the route `options`: the whitelist is small on purpose, and grows when an agent needs one.
- No model, key or network in the engine (D109).
- No multi-board or multi-user server. One session is one board, one agent.

## 9. Built and measured

*(Filled in as the stages land.)*
