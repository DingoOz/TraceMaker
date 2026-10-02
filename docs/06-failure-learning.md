# 06 — Failure memory: learning from every failed attempt

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).

## 1. Requirement

"Try an optimal algorithm, but any time something doesn't work, store that information and use it to
improve the next attempt." TraceMaker implements this as a **failure memory** with four tiers. Each tier
borrows a mechanism with a published track record:

| Tier | Scope | Mechanism | Prior art |
|---|---|---|---|
| T0 | One search | Explain the failure: which objects bounded the explored region, where the closest approach was | Conflict analysis in CDCL SAT solvers |
| T1 | One run, all connections | History cost maps, targeted penalties, conflict graph, activity scores | PathFinder / VPR history cost; FGR; TritonRoute-WXL marker cost; VSIDS |
| T2 | One run, strategy level | Nogoods (do not retry what failed in the same configuration), escalation state, bandit over strategies, restarts that keep what was learned | Nogood learning; tabu search; ALNS operator weights; UCB/Thompson bandits; Luby restarts |
| T3 | Across runs and boards | Persistent knowledge base: escape templates, strategy priors per board type, warm-start maps, trained models | Experience replay; algorithm configuration (SMAC/irace); learned net ordering (DreamerV3+FR, PCBWorld) |

## 2. T0 — explaining a failed search

When an A* search exhausts its window or budget, the router builds a `FailureRecord`:

```cpp
struct FailureRecord {
  ConnId conn; NetId net; Stage stage; Rung rung; StrategyId strategy; uint32_t attempt;
  Cause cause;            // BLOCKED, CUT_OVERFULL, PIN_INACCESSIBLE, NO_VIA_SITE, WINDOW_INFEASIBLE,
                          // BUDGET, DRC_ON_INSERT, LENGTH_RULE, ECO_REJECTED
  Rect region; LayerMask layers;
  uint64_t region_sig;    // Zobrist signature of the window (§3.3)
  SmallVec<ObjRef, 16> blockers;      // objects on the boundary of the closed set, ranked by contact
  SmallVec<NetId, 8>   blocker_nets;
  Point closest_a, closest_b;         // nearest approach of the two search trees (bidirectional)
  Segment cut; int32_t cut_demand, cut_capacity;   // when a Maley cut was evaluated
  WorkUnits spent; Score score_before;
};
```

- **Blockers** come for free from bidirectional A*: the boundary of each closed set is a closed curve
  of obstacles. The two curves around the source and target are the "boxed-in" explanation (Contour).
- **Boundary owners** are ranked by how much of the boundary they form; the top ones are the
  conflict explanation, analogous to a learned clause in a SAT solver.
- The record is appended to an append-only **failure log** (also streamed to the viewer, where failed
  attempts show as red ghost paths with their blockers highlighted).

## 3. T1/T2 — using failures within a run

### 3.1 History cost (PathFinder) — global and detailed

```
c(e)  = b(e) + α · h(e) · p(e)
p(e)  = 1 + pres_fac · max(0, use(e) + w − cap(e)) / cap(e),   pres_fac = 0.5 · 1.3^k (capped)
h(e) ← 0.9 · h(e) + overflow(e)   each iteration; α keeps present overflow dominant (BoxRouter)
```

As in the clean-sheet doc §5.3. Shared by global routing, detailed routing and repair.

For DRC markers in detailed repair, use the TritonRoute-WXL scheme as implemented in OpenROAD `drt`: each
marker adds +10 to a saturating per-cell byte counter, the counter decays ×0.95 per repair pass, and a cell
with a non-zero counter adds `marker_cost × step_length` (default `marker_cost = 32`, shape cost 8). The
repair schedule varies window size (7/5 tiles), offset, marker cost (0 → 1× → 2×) and rip-up mode
(all / DRC / near-DRC), and portfolio arms run marker cost ×½ and ×2.

### 3.2 Targeted penalties from failure records

Plain history only marks *over-used* resources. A failure record also says *where* a connection got stuck.
- Along the **boundary of the closed set** near the closest approach, add a penalty to cells owned by the
  ranked blocker nets: `learned(cell) += κ · rank_weight`. Next time, *those blockers* see a higher cost
  for staying there, so they move aside (like history, but aimed at the cause).
- Along an **over-full cut**, raise the cost of every net crossing the cut that could route elsewhere.
- Decay: `learned ← 0.85 · learned` per repair iteration, so stale lessons fade.

### 3.3 Region signatures and nogoods

- The window around a failure is described by a 64-bit **Zobrist hash**: one random key per (cell-block,
  layer, owner-class) where cell-blocks are 4×4 lattice cells and owner-class ∈ {free, own net, fixed,
  rippable-foreign}. XOR-updating the hash when copper changes is O(changed blocks), so every window's
  signature is maintained incrementally.
- A **nogood** is `(conn, rung, strategy, params_hash, region_sig) → FAILED`. Before trying a rung, the
  router checks the nogood store (an `ankerl::unordered_dense` set, ~16 B/entry). A hit skips the rung:
  *the exact same attempt in the exact same situation is never repeated.*
- Any copper change in the window changes the signature, so the attempt is allowed again once something
  relevant changed (no stale tabu).
- **Aspiration**: a nogood is ignored if the global score improved by more than a threshold since it was
  recorded (from tabu search).

### 3.4 Conflict graph and ordering

- Weighted directed graph `blocked_net → blocker_net`, weight bumped per failure, decayed per iteration.
- **Victim selection** in rip-up prefers blockers with high in-weight from many failing nets.
- **Net ordering**: an activity score per net (VSIDS-like): `act(n) += bump` on each failure of n,
  `bump *= 1/0.95` per iteration (equivalent to decaying all). Nets are routed in descending activity, so
  hard nets get first pick of resources on the next pass.
- **Precedence learning**: if A failed because of B, and B rerouted cheaply after a rip-up, add a soft
  precedence "A before B" for the rest of the run.

### 3.5 Strategy selection (bandit)

- Arms: rung variants and parameters (cost schedules, window sizes, pitch, via cost, bend cost, order
  heuristic) and placement-ECO move types.
- Reward = score improvement per work unit. **Discounted Thompson sampling** (or discounted UCB) per
  (stage, failure cause), so the router learns, on *this* board, which strategy fixes which kind of failure.
- Priors come from T3.

### 3.6 Restarts that keep the lessons

When the repair stage stalls (failure predictor says zero markers is unreachable in budget), the scheduler
may **restart** detailed routing from the global solution on a Luby schedule, keeping history, learned
penalties, activity, precedence and nogoods — the routing analogue of a SAT restart that keeps learned
clauses. Portfolio branches share T1 lessons only at deterministic synchronisation points.

### 3.7 Infeasibility cores → placement

An R4 exact window solve that returns *infeasible* yields a minimal conflicting set of boundary terminals.
That core plus the R6 cut proof is the ECO placement request (doc 04 §4). A rejected ECO move is itself a
failure record (`ECO_REJECTED`) keyed by the cut signature, so the same move is not proposed again.

## 4. T3 — the persistent knowledge base

SQLite file (WAL mode) at `~/.local/share/tracemaker/kb.sqlite`, also exportable to share between machines.

| Table | Key | Content | Used for |
|---|---|---|---|
| `boards` | board content hash | features (layers, part count, pin density, BGA count, net count), outcome | similarity search |
| `runs` | run id | settings, seed, version, metrics | regression tracking |
| `strategy_stats` | (feature cluster, stage, cause, arm) | Beta(α, β) or mean/var of reward | bandit priors |
| `escape_templates` | (footprint geometry hash, rule class, layer count) | escape stubs, via sites, layer per pin | first candidate in escape planning |
| `warm_start` | board hash (+ ECO diff) | final history map, activity, net order, corridors | rerouting the same board after edits |
| `failure_hotspots` | footprint-pair geometry hash | typical failure causes and fixes | ECO move ranking |
| `models` | name, version | trained weights (ONNX) | learned net ordering / congestion prediction |

**Learned models (later phase)** — trained offline with PyTorch from benchmark runs:
- **Congestion predictor**: a small CNN over RUDY/pin-density/blockage maps that predicts global-routing
  overflow; used in placement step (B) as a faster routability term.
- **Net-order policy**: a GNN over the netlist/placement scoring "route this connection next"; used as one
  ordering arm in the bandit, never as the only one. (Prior art: DreamerV3+FR and PCBWorld show RL over
  ordering decisions can lift completion; TraceMaker keeps it optional and benchmark-gated.)

## 5. Visibility

Every lesson is visible: failure ghosts, blocker highlights, history heatmaps, the conflict graph, bandit
arm probabilities and the nogood count are panels in the viewer (doc 09). The report lists every unrouted
connection with its full failure history.

## 6. Novelty note

No published VLSI or PCB detailed router uses CDCL-style nogood learning (closest: conflict-driven ASP for
optical routing, 2015). The nogood store and infeasibility cores (§3.3, §3.7) are therefore TraceMaker's own
contribution, and their benefit must be shown by ablation in the benchmark (roadmap M5 gate).

## 7. Guarantees

- Learning changes *costs, order and which attempts are made*, never the legality check. Nothing learned
  can create a DRC error.
- All learning updates are deterministic functions of (seed, events in deterministic order).
- Memory bounds: failure log capped (oldest summarised), nogood store capped with LRU on aspiration.
