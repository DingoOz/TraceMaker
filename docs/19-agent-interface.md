---
status: partly-built
code: [src/session]
---

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
| 4 Transactions | A `route` is kept only if it routes something and, with `verify: "refuse"` (the default), adds no DRC error relative to the state it starts from; otherwise the state is unchanged and the result says why. `report` keeps it and names the added errors, `off` skips the check. `undo`, `restore` and `history` go back to any state |
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
- **Caps.** At most 512 open connections listed per call, 64 nets per `route`, `work` at most 50 M per call (times up to 8 variants), 256 states kept, a request line of at most 1 MB. Start an unattended agent with `--work-limit`: without it nothing but the per-call caps bounds what an agent can spend.
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
| `route` | `nets` (names; default: every net with an open connection), `work` (default 2 M, at most 50 M), `variants` (1 to 8, default 1), `seed`, `hints` (an advice object: `first_nets`, `priority`, `layers`, `regions`), `options` (a whitelist of booleans: `soft_zones`, `via_in_pad`, `diff_pairs`, `vias_off_pads`, `escape_plan`), `verify` (`refuse` by default, `report`, `off`), `commit` (default true) | `committed`, `reason` (when not), `state`, `connections` and `routed` and `open_after` (for the nets this call named), `open_total` (the board's), the new copper per net (tracks, vias, length), `added_errors` (DRC types, relative to the state the call started from), the connections still open with `reason`, `why`, `tried` (a search ran or the pin was ruled out; false: never attempted), `rips`, `fails`, `blockers`, and `work` (`used`, `total_used`, `left`). A call can spend more than `work` (proofs of boxed-in pins and the clean-up count on top; observed up to 1.6 times with one router) |
| `rip_net` | `net` | the session copper of that net removed (a new state); how many tracks and vias; refuses a net with none |
| `check` | `full` (default true), `refill` | unconnected items and DRC errors by type, the types and counts the session added relative to the input board, and for those up to five `examples` each (items with positions: which net, where) that the input board does not have. `refill` judges with the zones refilled, as `kicad-cli --refill-zones` does (default: on once a route with soft zones was kept; a route with soft zones is verified refilled, because the router counts on the refill) |
| `stuck_report` | `redact` | doc 17's report for the last `route` (reasons, crowding, parts at the open ends, `advice_schema`) |
| `inspect_pad` | `pad` (`REF.PIN`), `radius_mm` (0.1 to 10, default 1.5) | read-only: the pad (net, position, layers, size, distance to the box of the outline), the open connections that end on it with the last route's reason and blockers, the other pads within the radius (at most 40, nearest first), and the copper within it (the board's and the session's, per net and layer, nearest first, at most 30 rows; `within_pad_area` marks another net's copper that lies in the pad's own area) |
| `inspect_net` | `net` | read-only: its pads (at most 60), the board's and the session's tracks and vias on it, the session's length per layer, the open connections, the box it spans, and its class width and clearance |
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
  To attach it to Claude Code, for example: `claude mcp add tracemaker -- /path/to/tracemaker session /path/to/board.kicad_pcb --out /path/to/routed.kicad_pcb`
  (other clients take the same command and arguments in their MCP server configuration). `--log FILE` records the session for `--replay`, and `--view` lets a
  person watch the agent route in the browser viewer. The server speaks on standard input and output only; the router's own text goes to standard error.
  An agent that has a shell tool but no MCP client can drive a session with `agents/relay.py`: `start NAME BOARD OUT` holds a session under a small daemon,
  `call NAME TOOL '{...}'` sends one tool call and prints the answer (`result NAME` fetches an answer that took longer than `--wait`), `stop NAME` ends it. Every
  request and answer is also appended to the session folder's `transcript.jsonl`, which `bench/agent_score.py` reads to score the board the agent saved.
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

Records: [`bench/records/m18/`](../bench/records/m18/README.md).

### 9.1 Built

- `src/session/` (`session.{hpp,cpp}`, `transport.{hpp,cpp}`): the ten tools of §4, the states, the log and its replay, MCP and JSON-lines transports;
  `tracemaker session`. One engine change: `RouterOptions::plan_only` (plan the connections and stop), used to list a state's open connections; off by default.
- `agents/session_client.py` (a Python client), `agents/repair_agent.py` (the scripted baseline, two policies), `bench/agent_eval.py` (the three arms),
  `bench/sets/srj20-agent-heldout.txt`.
- Tests: `tests/test_session.cpp` (12 cases, 176 assertions: listing and routing, equality with the plain router, rip/undo/restore, dry runs and refusals, hints,
  the work limit, `save` only to `--out`, replay and a changed log, thread-count independence, every listed tool dispatchable, both transports on bad
  requests), `tests/integration/session_cli.py` (the real binary over both transports, a net name that carries an instruction, replay to the same bytes, equality
  with `route --variants 1`). ASan + UBSan and TSan clean on the session tests; the CLI test also under ASan + UBSan.

### 9.2 The first call is the command line's

`route --work 20000000 --variants 8 --threads 8` and the session's first `route` with the same arguments on `srj20` sample054 give 56 routed, 596 tracks, 154 vias
and 69,573,838 work, both. The plain and control columns of the evaluation reproduce doc 17 §8.1's totals exactly (430 / 431 and 439 / 450).

### 9.3 The stop-rule test with a scripted agent: not met

Sixteen boards (the even and odd halves of `srj20-m15fail4`), connections routed, summed:

| Arm | Even half (489 open) | Odd half (492 open, held out) |
|---|---:|---:|
| plain, 8 variants | 430 | 439 |
| control, 12 variants (the agent's compute) | 431 | **450** |
| agent, policy `tabu` (each stuck connection once) | 430 | 439 |
| agent, policy `persist` (retries with other blockers and seeds, plateau moves kept) | 430 | not run (confirmed on a fresh set, below) |

Policy `tabu` used 77 % of the control's work; none of its 43 repair rounds was kept. Policy `persist` used 95 % and kept 150 plateau rounds that never
reduced the open connections. Neither routed one connection more than the plain run on any of these 16 boards. The control's gain on the odd half (+11)
comes from the four extra seeds, which a repair of three stuck connections does not reproduce. Every unrouted connection on these boards is `boxed_in` (a ball
fenced by its neighbours' copper), and the proof of it is the expensive part of a repair call (8.6 M flood cells against 0.85 M expansions in one case).

Because `persist` was designed after the `tabu` result on both halves, it was confirmed on 16 boards no agent was developed on (`srj20-agent-heldout`), run once:

| Arm (16 held-out boards, 1,162 open at the start) | Routed |
|---|---:|
| plain, 8 variants | 960 |
| control, 12 variants (the agent's compute) | **968** |
| agent, policy `persist` (95 % of the control's work) | 967 |

**Stop rule: not met, by one connection.** A model-driven arm (§9.3.1) then tied the control. Exploratory reading by group: on the 8 boards where the plain run leaves only 3 to 6 connections open the agent gains
nothing (429, plain 429, control 433); on the 8 where it leaves 22 on average it routes 538 against the control's 535 and the plain run's 531. Eight boards, one
run: a lead, not a result. A repair loop earns something where there is something to repair, and the extra seeds of the control earn their gain where little is
left. Whether a model does better than this loop is untested.

#### 9.3.1 A model-driven arm

Four model agents (subagents, one brief, four boards each; the first call mandated to be the plain route; scored from the boards they saved, never from their own
reports; two sessions replayed from their logs byte for byte) drove the same 16 boards through the tools of §4, `inspect_pad` and `inspect_net` included:

| Arm (16 held-out boards, 1,162 open at the start) | Routed |
|---|---:|
| plain, 8 variants | 960 |
| control, 12 variants (the agents' compute) | 968 |
| scripted agent, policy `persist` | 967 |
| **model-driven agents** (94 % of the control's work) | **968** |

**Stop rule: not met, by a tie** (968 against 968). The model never routed less than the plain run, gained +8 over it, used 94 % of the allowance, was better than the
control on 5 boards and worse on 6. It gained on boards where the scripted loop did not, and the loop gained where the model did not; one run each. The technique that
worked was single-net: look at the stuck ball and its target pad with `inspect_pad`, remove the nets beside it, route the stuck net alone. The removed nets then
mostly failed to return, so a cycle gained 0 or 1 and every board reached a plateau. It cost 615 k tokens for +8 connections; four more seeds give +8 for compute alone.
So: with this router and these boards, an agent matches what extra seeds give and does not exceed it, and by D118 nothing more is built for agents until the router can
tell an agent more than it does now (below). What the agents asked of the tools, and what was done, is in `bench/records/m18/README.md`.

### 9.4 What a session showed that the measurement did not

- **The engine's DRC and KiCad disagreed on two boards of this set** (`sample031`, `sample191`): the engine counted 3 added `copper_edge_clearance` errors where
  `kicad-cli` counted none. The router puts a track with its edge exactly on the centre line of an outline whose stroke is 0.05 mm and whose clearance rule is 0;
  `check_edges` (`src/drc/drc.cpp`) counted touching as a violation at zero clearance, KiCad does not for a straight track. **Fixed afterwards** (issue #42, D120: the edge
  threshold depends on the shape, frozen against KiCad in `tests/integration/edge_touch`); the numbers of this section were measured before the fix. It is why `verify`
  has the modes of §4 and why the agent's first call uses `report`; after it the engine and KiCad agree on `copper_edge_clearance` for the plain route of `sample031`, `sample007`, `sample191` and `sample039` (65, 72, 69 and 78; `bench/records/issue42`). The session's `refuse` mode was not re-run.
- **A model cannot choose blockers blind** (this is why `inspect_pad` and `inspect_net` exist, §4). A session driven by hand on `sample119` (`bench/records/m18/README.md`) swapped one stuck connection for another and
  never reduced the open count: the tools name nets and pads, and say nothing of what lies around a pad. And `stuck_report` after a route restricted to a few
  nets has no crowding (the other nets are locked copper, not routed copper of that run).
- **Listing the open connections costs a router set-up** per state. Fine on these boards; on a large board it is the reason for the daemon.

### 9.5 Not built, and what follows

- What the agents asked for and the router cannot yet say: which end of a boxed-in connection is fenced and which lanes and via sites are free; a dry run that names the
  minimal set of nets to remove to free a pin; "stopped at net X" and a reason with blockers for every open connection of a multi-net call. These are router
  questions (escape analysis per pad, doc 05 §14), not session ones; they are the thing to build if agents are worth another try.
- The daemon (`tracemakerd`) and the KiCad assistant that uses it (the other half of M18).
- Redaction in the session, `propose_path`, placement moves (§8).
