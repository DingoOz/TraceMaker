# Vias near pins (H1) and layer changes (H2): evidence from TraceMaker's own failing boards

Date: 2026-10-29. Code: worktree `/home/dingo/Programming/TraceMaker-speed`, main at db5950f (release 0.12.1). Boards: the routed
results of the `rel0120` release run (tiers B-D, 120 s, 8 variants). Analysis only: the router's source was not changed.

Two hypotheses came from looking at routed boards by eye:

- **H1.** Open connections are often blocked because a via, usually of another net, was placed too close to a footprint's pins and
  blocks their escape (fan-out).
- **H2.** Many open connections would route with two vias and a short run on another layer, but the router does not find this path.

**Short answer.**

- **H1 is partly true, and mostly in a form that is not "a via".** Copper of other nets within 2 mm of the pins is the main blocker.
  Removing it lets the connection route alone in 155 of 288 cases. That copper is mostly tracks, though.
  - Removing only the foreign vias within 2 mm rescues 46 of 288.
  - Every connection that a via removal rescues also routes when only the foreign tracks are removed and the vias are kept (43 of 43
    in the one-cause table).
  - Vias near open pins are closer than near connected pins of the same packages: median gap 0.54 mm against 0.82 mm. The effect is
    strong on two boards (CoreOne, kitspace_d20) and absent on the others.
- **H2 is not true of the search itself.** The A\* may place a via at every lattice point on every expanded state. Its cost-to-go
  field includes layer changes. The router's "boxed in" verdict is an exact proof over all layers and vias at lattice resolution.
  - Many open connections do route alone with 0-2 vias: 139 of 349 on the final boards.
  - They stay open in the release run for budget and ordering reasons. On most of these boards the run spends its whole budget in
    the strict first pass and never negotiates.
  - The connections that need more search do not need "two vias". They need long detours: median 6-7 vias and 1.3-1.7 times the
    straight distance, outside the strict pass's windows.
- **The PoE switch (the coordinator's added board).** On a copy routed with the 0.12.1 binary the open connections were GND pad to
  plane drops, not the "simple traces" the user saw in the live run.
  - Two are proved enclosed by WholeBoard's locked copper, at every pitch and option tried.
  - Two are trivial 1.2 mm stubs. They fail only because a pad-to-plane search has no heuristic and floods to its 3 M expansion cap.

Measured and inferred results are marked as such throughout.

---

## 1. How vias are placed and costed today (from the code)

All references are to `src/route/router.cpp` unless another file is named. Line numbers are at db5950f.

### 1.1 Via sites: everywhere on the lattice

The A\* considers a through via at **every expanded lattice state**, from every layer to every other usable layer. The code is at
1877-1925.

- `via_useful` (1880) only skips the legality check when no other layer could be improved at the bare via cost.
- `via_cost_at` (952-986) decides legality. Through the per-class cache it uses `Obstacles::fixed_via_code`
  (`src/route/obstacles.cpp:886`), which checks the via disk against fixed copper on all layers. It then uses
  `Obstacles::routed_via_state` (`obstacles.cpp:1117`) for this run's routed copper.
- So the search is a full 3-D lattice search (x, y, layer), and a layer change is never confined to special cells.
- There are two exceptions, both opt-in:
  - `--via-drops` (1076-1150) adds off-lattice via sites 0.7-1.5 mm out from dense-package pins.
  - `--via-in-pad` (1150-1247) adds a via in a pad.

**Lattice margin on vias (code finding, not measured).**

- `via_cost_at` checks the via disk inflated by `margin = 0.71 * pitch` (959) and has **no "tight" fallback**.
- Track points have such a fallback. A point that is legal only without the margin is allowed at a cost of 3 pitches
  (`point_state` 862-895, `cell_cost` 949).
- A via sits exactly on its lattice point, so the margin looks unnecessary for the via itself.
- With the winning variant on four of these boards ("cheap vias, 2x pitch on large boards"; pitch 0.11-0.2 mm) the extra clearance is
  0.08-0.14 mm.
- This could not be tested without changing the source. The half-pitch experiment (FINE, §3.3) is only a weak proxy, and it was
  memory-limited.
- The EtherCAT review (`reports/realworld/ethercat_qfn_analysis.md` §4, run SEEDFULL) found the same thing from the other side. There
  the designer's +3V3 vias "fit there, off-lattice" where the router proved the pins enclosed.

### 1.2 Via cost against track cost

- **The base price.** `via_cost_mm = 3.0` (`router.hpp:65`): one via costs as much as 3 mm of track. The portfolio varies it:
  - ×0.4 in variant 5 ("cheap vias", `router.cpp:6384`);
  - ×2.5 in variant 7 ("dear vias", 6386);
  - critical nets pay `critical_via_mult` (1689).
- **The clean-up raises it to ×10** (`via_cost_mult`, 4492, 4567) to save vias. It runs only when the state is a best state (6124).
- **In the A\* the via cost only orders the search.** The search is optimal in its window, so the cost changes which path wins, not
  whether one is found.
- **Experiment.** A via cost of 0.3 mm (CHEAP) rescued fewer connections than plain extra work (BIG): 40 against 78 (§3.3).

### 1.3 The heuristic and layer changes

- **Small windows** (under 60,000 lattice points × layers, 1694) use the octile distance to the target pad centre (1707-1719). It
  ignores layers and vias, and it is admissible.
- **Large windows** use a cost-to-go field (`build_field`, 1249-1340, on the GPU or the CPU reference).
  - It is a multi-layer distance over cells not known to be blocked by **fixed** copper.
  - Via steps are allowed where the via cache does not already say "blocked" (1295-1300), at the same `via_cost` as the A\* (1306).
- **So a per-layer, via-aware cost-to-go already exists.** It ignores this run's routed copper (which is why it stays a lower bound),
  and it never prunes (1712).
- **Pad-to-plane connections (`zone_target`)** get `h = 0` (1716), no field (1694) and no late reachability proof (1781). Those
  searches are uninformed floods; see the PoE board in §5.

### 1.4 Windows and caps

- **Windows** are the bounding box of the two pad centres, grown by a margin on every side (2605-2650): first 2 mm + length/4, then
  6 mm + length/4.
  - The strict first pass tries only those two windows (2642).
  - Negotiated searches add a 20 mm window (`soft_attempts = 3`).
  - The whole-board window (margin 1,000 mm) is used only with `--soft-attempts 4`.
  - The window includes space behind the source: the margin is symmetric.
- **Expansion caps.**
  - Each search has a cap of 3 M expansions (`router.hpp:44`).
  - The reverse probe from the target is capped at 60,000 expansions (2673).
  - The late reachability proof starts after 100,000 expansions and visits at most 10,000 flood cells (`router.hpp:49`, 1781).

### 1.5 Exactness of "boxed in"

- The pre-check flood (1620-1660) and the late proof (`late_reach_proof`, 1405-1500) allow every planar move and every via landing
  that the A\* allows. A "no path" from them is exact over all layers at lattice resolution (doc 05 §62).
- When the A\* exhausts its own region, "boxed in" is exact up to its turn limit. Turns of 135° or more are forbidden (1853).
- So **a connection reported "boxed in" had no lattice path at all, with any number of vias**, against the copper present at that
  moment.

### 1.6 Turn handling and fast bends

- With `bend_states = false` (the "fast bends" variants 1, 2, 3, 5, 6, 7) a lattice point holds one state with its parent direction
  (1522). The ≤ 90° turn rule can then prune paths in principle.
- The study measured the opposite (§3.4): at equal work, fast bends route more connections alone than exact bends. On these boards,
  incompleteness did not show.
- A via landing resets the direction (`kNoDir`), so a via also allows any turn.

### 1.7 A keep-out or cost for foreign vias near pins

**There is none in the default router.** What exists:

- **`--keep-vias-off-pads`** (`obstacles.cpp:396`). It keeps vias off small SMD pads at clearance only, with no distance beyond the
  rule. Off by default.
- **The escape plan (`escape_plan`).** It reserves corridor cells for the pins of dense packages (`plan_escape_reservations`, 5317).
  - `via_cost_at` refuses a via of another net on a reserved cell of *any* layer in a strict search (979-985), and charges
    `reserve_pen` in negotiation (186-191).
  - So it is a via keep-out on 2 mm corridors.
  - It is on only in variants 3 and 6 (6381, 6385). Those variants won on TCKB and mini_mass, not on CoreOne, LeeChee, MonApollo or
    Aleste.
- **D143's "pin halo"** (2 pitches per step within 0.3 mm of another part's pad) was measured and dropped: 1,383 → 1,394 open
  (doc 05 §55). It applied to tracks; a via-only halo was not tried.
- **`end_crowding`** (2571-2590) counts other nets' routed copper within 1 mm of a pad. It only chooses which end a strict search
  starts from; it does not keep copper away.

### 1.8 Ripping vias near pins

- **In negotiation, yes.** A soft search may cross other nets' routed vias at `soft_unit` cost (`routed_via_state` with soft, 979).
  Those connections are then ripped.
- **But negotiation often never runs.** On TCKB, mini_mass, Aleste and kitspace_d20 the release run spent 0 expansions in negotiation
  (`route.json` work profile; §2). Doc 05 §55 found the same for 17 of the 33 failing boards.
- **The clean-up (`optimize_vias`, `lns_vias` 4566) moves vias to save vias and length.** It does not move them to free a pin, and it
  runs only on a best state.

---

## 2. The boards and how their open connections failed in the release run

| Board | Copper layers | Winning variant (pitch) | Open | Reasons in the run (`route.json` failures) | Negotiation expansions |
|---|--:|---|--:|---|--:|
| TCKB_Kicad_TCKB | 2 | jittered order, escape plan (0.1) | 18 | 15 boxed in, 3 not attempted | 0 |
| mini_mass_prog_bench_prog_rig | 4 | cheap crossings, escape plan (0.065) | 11 | 7 boxed in, 2 budget, 2 not attempted | 0 |
| CoreOne-xCORE200-Original_CoreOne | 4 | cheap vias, 2x pitch (0.11) | 28 | 28 boxed in | 3.9 M |
| LeeChee_1800 | 2 | cheap vias, 2x pitch (0.15) | 29 | 20 boxed in, 8 ripped and not routed again, 1 budget | 7.9 M |
| MonApollo_analog-board | 2 | cheap vias, 2x pitch (0.15) | 73 | 53 boxed in, 19 no path in window, 1 ripped | 0.1 M |
| Aleste-520EX_aleste | 2 | cheap vias, 2x pitch (0.2) | 228 | 105 no path in window, 28 boxed in, 95 not attempted | 0 |
| kitspace_d20_tri_r0.3 (tier C) | 4 | exact bends, jittered (0.035) | 139 | 78 boxed in, 9 no path in window, 52 not attempted | 0 |

**Measured.** Of the 526 open connections:

- 229 were "boxed in" in the strict first pass, mostly with no "negotiated:" clause: the run never tried them again with rip-up.
- 133 were "no path in window".
- 152 were never attempted before the time ran out.

**Inferred from §1.5.** The 229 boxed-in connections had **no path at all with any number of vias** against the routed copper of the
moment. H2's "a two-via path exists" cannot hold for them in that state. They needed other nets' copper to move.

The vias on these boards are large next to the packages' pitch:

| Board | Vias (size/drill) | Densest package pitch |
|---|---|--:|
| CoreOne | 0.8/0.5 | 0.4 mm (U801, xCORE) |
| MonApollo | 1.016/0.508 and 1.27/0.635 | — |
| Aleste | 1.0/0.5 | — |
| LeeChee | 0.6/0.4 | — |
| kitspace_d20 | 0.46/0.2 | — |
| mini_mass | 0.7/0.3 | — |

---

## 3. Experiments

### 3.1 Method

The program is `build/via-layer-study/viastudy.cpp`. It links the release libraries and only calls the public `Router` API.

**Set-up for each board:**

- Read the routed board, with rules from the fixture beside it (as `bench/failure_causes.py` does).
- Lock all copper, as `tracemaker causes` does. The input copper is then fixed and nothing else is routed.
- List the open connections (`plan_only`).
- Run the escape analysis on the bare board.

**Then, per net, route that net alone** (every other net in `skip_nets`; seed 1; CPU fields; default `RouterOptions`, that is,
variant 0 "exact bends" at the automatic pitch). The copies and options:

| Run | What changes | Work |
|---|---|---|
| A | as it is | 2 M |
| V05 / V1 / V2 | foreign vias whose gap to an end pad is below 0.5 / 1 / 2 mm removed; their tracks stay | 2 M |
| T2 | foreign tracks within 2 mm removed; vias stay | 2 M |
| VT2 | both within 2 mm removed | 2 M |
| BIG | as it is, with `max_expansions` 20 M and `soft_attempts` 4 (whole-board window in negotiation) | 20 M |
| CHEAP | as it is, via cost 0.3 mm, whole-board window | 4 M |
| FINE | half pitch | 6 M |
| SWAP1 | for connections that V1 rescues: all copper of the nets that owned the removed vias deleted, then this net (first) and those nets routed together; it succeeds if this connection routes and those nets lose nothing | 6 M |

**A second pass** runs on all connections:

| Run | What changes |
|---|---|
| FB | fast bends |
| W | the winning variant's settings: fast bends, via cost ×0.4, 2x pitch on large boards, half on small ones |

**Coverage.** Every open connection of TCKB, mini_mass, CoreOne, LeeChee and MonApollo was run. Aleste and kitspace_d20 used
deterministic samples: 66 of 172 nets (95 of 228 connections) and 50 nets (95 of 139 connections).

**Gaps** are measured edge to edge with exact pad shapes (`drc::pad_shapes`, `geom::gap`).

**Limits of the method:**

- One connection alone on a locked board is easier than all of them together, so rescue counts are upper bounds. This is the same
  caveat as D143.
- A 2 M exact-bend run is a small budget. The FB pass shows how many A failures were only budget (§3.4).
- FINE ran on four boards only (TCKB, CoreOne, MonApollo, plus LeeChee before it was killed). At half pitch the per-router lattice
  arrays reached about 43 GB, and a memory guard killed those runs. LeeChee and mini_mass were rerun without FINE, and Aleste and
  kitspace_d20 never ran it.

### 3.2 H1: what has to be removed for an open connection to route alone (measured)

Each connection gets the first match, in this order:

1. **alone:** routes alone in A, FB or W at 2 M.
2. **search:** routes with BIG, CHEAP or FINE.
3. **via 0.5 / 1 / 2:** routes with foreign vias removed within 0.5, 1 or 2 mm.
4. **tracks 2:** routes with foreign tracks removed within 2 mm, vias kept.
5. **both 2:** routes with both removed.
6. **none.**

| Board | Sampled | alone | search | via ≤0.5 | via ≤1 | via ≤2 | tracks ≤2 | both ≤2 | none | via rescues also rescued by tracks only |
|---|--:|--:|--:|--:|--:|--:|--:|--:|--:|--:|
| TCKB | 18 | 5 | 1 | 0 | 0 | 0 | 3 | 1 | 8 | – |
| mini_mass | 11 | 3 | 0 | 2 | 0 | 0 | 4 | 0 | 2 | 2/2 |
| CoreOne | 28 | 6 | 0 | 5 | 4 | 1 | 4 | 0 | 8 | 10/10 |
| LeeChee | 29 | 21 | 0 | 0 | 2 | 2 | 0 | 0 | 4 | 4/4 |
| MonApollo | 73 | 26 | 1 | 2 | 0 | 3 | 25 | 0 | 16 | 5/5 |
| Aleste (sample) | 95 | 66 | 12 | 0 | 0 | 0 | 3 | 0 | 14 | – |
| kitspace_d20 (sample) | 95 | 12 | 0 | 6 | 6 | 10 | 61 | 0 | 0 | 22/22 |
| **Total** | **349** | **139** | **14** | **15** | **12** | **16** | **100** | **1** | **52** | **43/43** |

Counts per experiment over the 288 connections that failed A, not first-match:

| Experiment | Rescued |
|---|--:|
| V05 | 16 |
| V1 | 29 |
| V2 | 46 |
| T2 | 154 |
| VT2 | 155 |
| BIG | 78 |
| CHEAP | 40 |
| FINE | 3 |

**Reading:**

- **Foreign vias within 2 mm are a sufficient fix for 46 of 288 (16 %), and never a necessary one.** In all 43 first-match via rescues,
  removing only the tracks (vias kept) also routes the connection.
  - The vias that sit near open pins are the ends of other nets' fan-outs and stubs. The tracks that lead to them block the escape
    just as much.
  - "The via" is the visible part of a blocking fan-out, not a separate cause.
- **Removing foreign tracks within 2 mm (vias kept) rescues 154 of 288.** That is the largest single lever. It agrees with D143's
  `through_traffic` + `package_fanout` classes (779 of 1,320).
- **SWAP1 (can both the open net and the via owners be routed if the open net goes first?)** succeeded in 5 of 17 tries. 6 more were
  skipped because the owners were large nets (over 60 pads). In most cases the via owner needs the same space: moving its via is not
  free.
- **Neck-down matters at the 0.4 mm package.** On CoreOne, 5 of the 10 V1/V2 rescues used the neck-down via or width. The class via is
  0.8 mm; the neck-down via is 0.5/0.3.

**Distance of the nearest foreign via, pads of dense packages that hold an open end (measured):**

| Board | Open ends | Median gap, open pads | Nearest via of a net with a pad on the same part | Median gap, connected pads of the same parts |
|---|--:|--:|--:|--:|
| TCKB | 21 | 0.48 mm | 21/21 | 0.66 mm (n = 58) |
| mini_mass | 18 | 0.56 | 18/18 | 1.00 (126) |
| CoreOne | 29 | 0.31 | 28/29 | 0.65 (146) |
| LeeChee | 30 | 0.66 | 30/30 | 1.13 (78) |
| MonApollo | 79 | 1.08 | 73/79 | 0.89 (295) |
| kitspace_d20 | 75 | 0.26 | 21/75 | 0.24 (32) |
| all | 252 | 0.54 (gap < 0.5 mm: 45 %) | | 0.82 (gap < 0.5 mm: 30 %; n = 735) |

- **Open pins have a foreign via closer than connected pins of the same packages** on TCKB, mini_mass, CoreOne and LeeChee, but not on
  MonApollo or kitspace_d20.
- **The nearest via is almost always of a net that also has a pad on the same part.**
  - The package's own neighbours' dog-bones and supply drops (CoreOne: /XMOS200/DVDD, IOVDD) block, not through traffic.
  - Caveat: on a 128-pin MCU most nets touch the part, so this split says less than it seems.
- The pattern matches the EtherCAT review: "+3V3 ... with vias in the other pins' escape zone".

### 3.3 H2: does a path with layer changes exist, and why is it missed? (measured)

**Routes alone.** 139 of 349 open connections route alone on the final board (A, FB or W at 2 M work). Their vias:

| Vias | 0 | 1 | 2 | 3 | 4 | 5 | 6+ |
|---|--:|--:|--:|--:|--:|--:|--:|
| A routes | 4 | 11 | 22 | 5 | 8 | 4 | 7 |

- These are the user's "would route easily with two vias".
- They were open in the run because of ordering and budget, not because the search cannot change layers:
  - "not attempted" (152 connections);
  - "ripped up and not routed again" (LeeChee 8);
  - "boxed in" against copper routed earlier, with no negotiation pass to move it (§2).

**More search** (BIG: 20 M work, a 20 M cap and the whole-board window) rescues 78 of the 288 A failures. Their routes are **not**
"two vias and a short run":

| Board | Rescued by BIG | Median vias | Median length / straight distance | Why A failed |
|---|--:|--:|--:|---|
| TCKB | 4 | 12 | 1.31 | boxed in (target) 3, budget 1; 3 of 4 used the neck-down |
| CoreOne | 2 | 4 | 1.21 | boxed in (target); both necked |
| LeeChee | 8 | 6 | 1.13 | out of budget 8 |
| MonApollo | 10 | 6.5 | 1.59 | boxed in (target) 5 (all necked), budget 3, no path in window 2 |
| Aleste | 53 | 7 | 1.67 | budget 24, not attempted 15, no path in window 12 |

- **Example (Aleste).** `/RAM/MA1` D16.7 (335.28, 152.40) → D8.7 (360.68, 152.40) is 25.4 mm apart in a straight line. It failed with
  "no path in window" and routes with BIG over **217.5 mm with 4 vias**. Only a whole-board window holds a path that leaves the
  region between the parts and comes back.
- **Via cost:**
  - CHEAP (via cost 0.3 mm, 4 M) rescues 40, a subset of what extra work alone does.
  - Its routes use even more vias (median 6-10).
  - A low via cost does not unlock paths that the search could not find.
- **Pitch.** FINE (half pitch, 6 M) rescues 3 (TCKB 2, MonApollo 1) where it ran.
- **No path at all.** 52 connections route in none of the copies. Their BIG verdict is mostly "boxed in" (source or target): no path
  at lattice level even with the whole board and any number of vias.

**Inferred.**

- **Literal H2 is not supported.** None of the evidence shows a short two-via path that the strict search missed through its heuristic
  or via cost.
- **What the measurements show:**
  - Many connections route with 0-2 vias when tried alone, and the run did not try them again or had no budget left.
  - Pins boxed in by copper routed earlier are proved enclosed over all layers.
  - Some connections need long, many-via detours that only the whole-board window or a larger budget finds.
- **Search limits that do bite:**
  - the 2-window strict pass and the 20 mm negotiated window ("no path in window": 133 in the run, 105 of them on Aleste);
  - the work budget.

### 3.4 Exact bends against fast bends at equal work (measured)

| Board | Sampled | A (exact bends) | FB (fast bends) | W (winning variant) | Only A | Only FB | Only W |
|---|--:|--:|--:|--:|--:|--:|--:|
| TCKB | 18 | 1 | 5 | 0 | 0 | 4 | 0 |
| mini_mass | 11 | 1 | 2 | 3 | 0 | 0 | 1 |
| CoreOne | 28 | 4 | 6 | 3 | 0 | 1 | 0 |
| LeeChee | 29 | 13 | 21 | 20 | 0 | 1 | 0 |
| MonApollo | 73 | 12 | 22 | 18 | 0 | 4 | 4 |
| Aleste | 95 | 18 | 53 | 41 | 0 | 15 | 13 |
| kitspace_d20 | 95 | 12 | 12 | 12 | 0 | 0 | 0 |

- At 2 M work, fast bends route more connections alone, and no connection routes only with exact bends. Exact bends have nine states
  per point, so the same work covers less ground.
- So the possible incompleteness of the fast-bend search (§1.6) did not show here.
- Many A failures are budget artifacts of the 2 M exact-bend run. §3.2 counts a connection as "alone" if A, FB or W routes it.

### 3.5 Illustrative connections (measured)

**CoreOne `Net-(R892-Pad1)`: R892.1 (158.85, 76.98) → U801.124 (152.28, 80.71), U801 at 0.4 mm pitch.**

- The run said "boxed in (target)", and so did A.
- The nearest foreign via to U801.124 is `Net-(R891-Pad1)`, the next pin's net, with a 0.26 mm gap. 9 foreign vias and 47 foreign
  track segments lie within 2 mm.
- Removing only that one via (V05) lets the connection route alone: 12.7 mm, 2 vias.
- Removing the tracks within 2 mm instead routes it on one layer: 8.1 mm, 0 vias.
- SWAP1 failed: with this net first, the 5 via-owner nets lose 2 connections.
- This is H1 as the user described it, but the same space is wanted by the neighbours' fan-out.

**CoreOne `Net-(R621-Pad2)`: U601.32 (178.15, 81.25; KSZ8081 at 0.5 mm) → R621.2.**

- The nearest via is `/Eth-KSZ8081/D1V2`, with a 0.25 mm gap.
- V1 routes it with 2 vias over 42 mm.
- SWAP1 succeeded: the D1V2 and ETH_TX_CLK nets route again with this one first. This is a via that could have been placed
  elsewhere.

**kitspace_d20 `colg_6`: D23.3 (112.48, 129.17) → D39.3 (114.73, 129.17), an LED matrix.**

- Foreign vias of `row_7` and `row_2` sit 0.15-0.16 mm from the pads.
- V05 routes it with 2 vias (4.4 mm); T2 routes it flat (2.5 mm).
- SWAP1 was skipped (162 pads on the owner nets).

**MonApollo `+12V`: U15.8 (75.89, 96.53) → R96.2.**

- "Fixed copper encloses the pin".
- No foreign via within 1.6 mm. Only removing tracks (T2) routes it: 14.4 mm, 2 vias.
- H1 does not apply here.

**Aleste `/RAM/MA1`.** The long detour in §3.3: 25.4 mm straight, 217.5 mm routed with 4 vias.

---

## 4. Changes these findings suggest

Each is tied to the evidence above, from the strongest evidence to the weakest.

1. **Free boxed-in pins by negotiation instead of leaving them open after the strict pass.**
   - Evidence: 139 of 349 open connections route alone with 0-2 vias, and 154 of 288 more route once foreign tracks within 2 mm are
     gone. 229 of the run's open connections were "boxed in" in a strict pass that never negotiated.
   - Proposal: a pin rescue step. When a pin is proved boxed in by routed copper, rip the routed connections whose copper lies within
     about 2 mm of it, route the boxed pin first, then re-route the victims, all as a transaction (D99).
   - The ripped set must include the owners of nearby vias. SWAP1's 5 of 17 shows that sometimes only re-ordering is needed.
   - This targets the largest measured class and needs no new search.
2. **Reserve a negotiation share of the budget.**
   - Evidence: on TCKB, mini_mass, Aleste and kitspace_d20 the run spent 0 expansions in negotiation.
   - D143 measured "strict pass stops at 70 %" as worse. A reservation tied to proved enclosures (item 1) is a different trigger and
     was not measured.
3. **A whole-board (or corridor-guided) negotiated attempt for connections whose 20 mm window was too small.**
   - Evidence: "no path in window" was 133 in the run, and BIG rescues 53 on Aleste with 1.3-1.7× (up to 8.6×) detours and 4-15
     vias.
   - Gate it on the window-edge flag the search already records (`touched_edge` / `proved_edge`), so that it costs nothing on
     enclosed pins.
4. **A via keep-out or cost in the escape cone of dense-package pins, for nets without a pad there.**
   - Evidence: on CoreOne and kitspace_d20, removing only nearby vias rescues 10 of 24 and 22 of 83, and open pins have closer vias
     (CoreOne median 0.31 against 0.65 mm).
   - The case is weaker than it looks:
     - the via owners are mostly the package's own nets;
     - no via rescue was via-only;
     - D143's track halo lost.
   - If tried, make it via-only, inside the pin's outward cone (the escape corridor geometry already exists in `route/escape.hpp`),
     and measure on CoreOne, kitspace_d20 and the quick tier.
   - The cheaper first test is `--escape-plan` in more variants on these boards. It already refuses foreign vias on corridor cells.
5. **A "tight" via state and off-lattice via sites.**
   - Evidence: the code (§1.1: vias need a 0.71-pitch extra margin, with no fallback), EtherCAT SEEDFULL, and CoreOne's neck-down share
     (5 of 10 via rescues).
   - Proposal: allow vias that fit without the margin at a cost, as tracks are allowed (`cell_cost` 949). Extend `--via-drops`-style
     off-lattice sites beyond dense SMD pins.
   - Unmeasured. It needs a source change and an A/B on the failing set.
6. **Give pad-to-plane searches a heuristic and the late proof.**
   - Evidence: the PoE board (§5). `zone_target` searches have h = 0, no field and no late proof (1694, 1716, 1781), so they flood to
     the 3 M cap.
   - A distance-to-nearest-fill-cell bound (fill cells of the net, per layer, plus via cost) is admissible.
7. **Not supported by this evidence:**
   - a new per-layer, via-aware cost-to-go (it exists, §1.3);
   - cheaper vias (CHEAP is a subset of BIG);
   - post-route via relocation alone (vias are never the sole blocker; their tracks block too). It might help as part of item 1, by
     moving a via together with its stub.

---

## 5. The 5-port PoE gigabit switch (WholeBoard rev16), added at the coordinator's request

**Board and set-up.**

- Board: `rev16/poe_gbe_switch_5p-2-placed.kicad_pcb`, with its `.kicad_pro` and `.kicad_dru`, copied to
  `build/via-layer-study/poe/`. The live route (PID 3840809, binary `TraceMaker-live`) and its directory were not touched.
- rev16 had no routed file yet, so the copy was routed with the 0.12.1 worktree binary: `--work 20000000 --threads 6 --diff-pairs
  --view --view-port 18801`, using the live run's `--diff-pairs`.
- It has 4 copper layers. GND (In1.Cu) and +54V (In2.Cu) zones have no fill; the router filled them in memory and made them soft.

**What is open (measured).**

- The placed board already holds WholeBoard's routing: 2,538 segments plus its vias (the ones inspected are locked).
- The router planned only **5 connections, all GND pads to the GND plane**: C115.2, C119.2, C123.2, U10.84, and one more.
- It routed 1 (3 tracks, 2 vias). The 4 open ones, from the run's own explanations:

| Pad | Position (mm) | Why |
|---|---|---|
| C115.2 | 176.99, 170.18 | search budget (3 M cap); negotiated: search budget |
| C123.2 | 175.99, 163.70 | out of budget |
| C119.2 | 155.40, 165.80 | boxed in; fixed copper encloses the pin |
| U10.84 | 164.30, 156.34 | boxed in, also in the escape and neck-down rung |

- **These are not the "simple traces" the user describes.** That may be because the live binary (`TraceMaker-live`) differs from
  0.12.1, or because the user watched a different state. This copy could not reproduce the user's observation, and these notes do not
  claim it.

**Single-variant experiments** (`poe/exp.sh`, `poe/q_*`; each with `--view` on ports 18802-18814):

| Run | Settings | Routed |
|---|---|---|
| unrouted copy, no `--diff-pairs` | `--variants 1 --work 20 M` | **3/5** in 2.9 M expansions |
| same, with `--max-expansions 30 M --soft-attempts 4` | | 3/5 |
| same, with `--via-cost-mm 0.3` | | 3/5 |
| same, with `--via-in-pad --via-drops` | | 3/5 |
| unrouted copy, **with** `--diff-pairs` (as the live run) | | **1/5**, the whole 20 M spent: 18.2 M of it in negotiation, mostly on C115.2 and C123.2 "search budget" |
| routed copy (P0-P6) | `--pitch-um 25`, a 30 M cap with a whole-board window, cheap vias, `--via-in-pad`, `--via-drops`, `--reach-check 2` | 2/4 in every run |

On the routed copy:

- **C115.2 and C123.2 route** as 1.17 mm straight F.Cu stubs to WholeBoard's locked GND copper beside C113. They need no via at all.
- **The other two stay boxed in under every setting.** They are C119.2 (here planned to C116.2) and U10.84.

**What the misses are (measured, with code).**

**C115.2 and C123.2: a planning and heuristic problem, not a layer problem.**

- In the portfolio run they were planned to the zone. A zone-target search has h = 0 (1716), no field (1694) and no late proof
  (1781), so it is a blind flood. It reached the 3 M per-search cap ("search budget") and was retried until the budget was gone.
- When the plan instead targets nearby copper of the net (the single-variant run without `--diff-pairs` restarted 46 times and
  re-planned), they route in a few thousand expansions.
- **Why `--diff-pairs` changes the plan was not resolved.** It changes how many restarts and re-plans happen: 0 with it, 46 without.

**U10.84 and C119.2: enclosed by WholeBoard's locked copper.**

- **U10.84.** It already has a locked 1.06 mm GND stub on F.Cu. Within 0.6 mm there are:
  - `+1V2A` on In2.Cu, 0.18 mm away;
  - `SPI_SCK` on In1.Cu, 0.56 mm away;
  - `U10_LED4_0/1` on F.Cu and B.Cu, 0.33-0.73 mm away.
  - The signal tracks on the plane layers cut the GND fill, so no GND fill cell can be reached.
- **C119.2** is ringed within 1 mm by `+1V2A_AN`, `+2V5A_AN` and `CHASSIS` on F.Cu and In1.Cu.
- **The enclosure holds under every setting** of the routed-copy runs above, including 25 µm pitch and the 30 M whole-board run.
- Locked items may not be moved (rule 6). These two are the board's input, not the search.

**The coordinator's specific questions.**

| Question | Finding |
|---|---|
| Does a path exist that needs 2+ vias and an initial move away from the target? | For the two enclosed pads, no path exists at all at lattice level (proved). The two that route need 0 vias. For the "move away" pattern in general, see Aleste `/RAM/MA1` in §3.3: 217.5 mm for 25.4 mm straight, found only with the whole-board window. |
| Does the window include space behind the source? | Yes: the margin is symmetric (2605-2650). For a zone target the window is the pad ± 2 mm, then ± 6 mm, and 20 mm in negotiation. A long "away first" detour needs the 20 mm or whole-board window, which the strict pass never uses (2642). |
| Does the cost-to-go field or heuristic pull toward the target? | It is admissible, so it orders expansions but never forbids moving away (A\* is optimal in its window). For zone targets there is no pull at all (h = 0); the cost is the flood. |
| The via cost? | It orders only. 0.3 mm changed nothing here (3/5 and 2/4 either way). |
| The strict-search expansion cap? | It bites for the zone-target floods (C115.2 "search budget"). Raising it to 30 M did not help, because the plan, not the cap, decides whether a cheap target exists. |
| What does `tracemaker causes` say? | Not usable on this board as it stands. `causes` builds default `RouterOptions` and does not fill or soften unfilled zones as `route` does, so it plans 169 pad-to-pad GND/+54V connections instead of 5 pad-to-plane ones. On those it reports 96 `no_path_bare`, 6 `near_and_far`, 5 `through_traffic` and 3 `channel` (`poe/poe.causes.json`). It needs the route job's plane handling before its classes mean anything on plane boards. |

---

## 6. Reproducing, and what ran

**Program and scripts** (in `/home/dingo/Programming/TraceMaker-speed/build/via-layer-study/`):

- `viastudy.cpp` (`viastudy`, `viastudy_fb`). The link line is in `libs.txt`; it links against `build/release` static libraries.
- `run_par.sh` (scheduler) and `memguard.sh` (kills a run above 40 GB).
- `analyse.py`, `analyse2.py`, `analyse3.py` (tables).
- `poe/` holds the PoE copy, its route, experiments and `causes` output.

**Raw output:**

- `out/<board>.json` and `out/<board>.fb.json` (per connection: end geometry and each experiment's result, vias, length per layer,
  neck-down, the failure text).
- `out/*.log`.
- `out/summary_*.txt`.

**Compute.**

- About 30 minutes of wall time on at most about 24 threads; about 26 for a minute or two when an old run overlapped.
- Two runs were killed by the memory guard at 43 GB: LeeChee with FINE, and mini_mass at 6 threads. Both were rerun without FINE or
  with 2 threads.
- `viastudy` calls the router library directly, so it had no viewer. Every `tracemaker route` run (PoE) used `--view` with its own
  port in 18801-18814.
