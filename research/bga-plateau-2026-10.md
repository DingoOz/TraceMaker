# Research: breaking the last-connections plateau on two-layer BGA boards (checked 2026-10-13)

Question: two-layer boards with a dense ball-grid package (0.35-0.8 mm pitch, 70-110 connections, partner pads often
10-17 mm away, through vias allowed). The lattice router with negotiated rip-up and transactions reaches about 90 %
of the connections and then stalls with 1-25 open, mostly outer-ring balls with far partners; more search time and
reseeding do not help. What does the literature offer for the last few connections, and what is it worth here?

This note adds to [`bga-routing-2026-10.md`](bga-routing-2026-10.md) (called "the first note") and does not repeat
it. What is already covered or measured there and in [`docs/16-bga-breakout-plan.md`](../docs/16-bga-breakout-plan.md)
§10-§12 (flow escape plans, escape corridors, longest-first and other orders, max-cut layer assignment, tscircuit
routes as guides, finer or different lattice pitch, victim orders, nested transactions, tidy sweeps, cooling, caches,
weighted A*, history and crossing-cost variants) is **not** proposed again.

Evidence tags as in the first note, plus one:

- **[V]** verified here: the primary text was downloaded and read (or the number was computed from a fetched text).
- **[A]** abstract, search-result summary or bibliographic record only. A lead, not a measurement.
- **[C]** calculated in this note from numbers given in the repository or in a [V] source.
- **[J]** my judgement. No data behind the number; it is there so the ranking is explicit and can be proven wrong.

**No experiment was run for this note.** Nothing here has been measured on TraceMaker. Every "estimate" is [J].

## 0. Ranking (summary)

Where each of the five questions is answered: hexagonal, 60-degree and any-angle lattices, section 2; topological and
ordered escape, section 4; layer assignment as a graph and via sites (dog-bone against via-in-pad), section 3; exact and
complete methods, LNS, cost schemes and shove, section 5.1-5.5; MAPF and VLSI routers, sections 5.5-5.6.

"1-3 open" means the boards that end with one to three connections open; in `dev/m15-notes.md` the best state over all
runs of the 76 srj20 boards that one variant at 50 M leaves open was 13 boards complete, 8 with 1 open, 6 with 2, 4 with
3, and every board of 70 or more connections stays at 8 or more open. So the "last few" class is 18 of the 63 boards that
stay open (the union of all runs completes 13 of the 76), and the large boards are a different problem.

| Rank | Candidate | Effort (C++ in this router) | Expected effect on boards at 1-3 open [J] | Evidence |
|---|---|---|---|---|
| 1 | **Route-pool exact selection**: keep many full candidate routes for the open connection and its neighbours, choose a conflict-free subset exactly (SAT or set packing), feed new candidates from the solver's conflict (column generation later) | 2-3 weeks | 25-40 % of such boards, about +3 to +8 boards on srj20; about 0 on the 28 big boards | Medium: route-variable SAT 66x faster than track-variable and able to prove unroutability [V]; the same idea in ILP form on ISPD-18 [A, slides]; path-based exact MAPF beats CBS [V abstract]. Nothing published on a BGA lattice |
| 2 | **Via-in-pad (via on own-net pad) as a rule-gated last rung** | 3-5 days | Unknown. Large where the fab allows it; zero where it does not | Geometry [C] and vendor articles [A]. Cannot be scored on the srj sets (their rules forbid it) |
| 3 | **Spatial and adaptive LNS** on the existing transaction code: ruin all nets within R of the open pad, re-create in randomised orders, accept equal or better, adapt neighbourhood type and size | 3-5 days | 10-20 %, about +0 to +4 boards | MAPF-LNS2 [V]; `--lns-open` in the repo was never reached at 50 M, so this is untested, not failed |
| 4 | **Region certificates and an exact lattice-window solver used as an oracle** (LP/cut bound first, SAT window second) | 1-2 weeks (certificate), 3-5 weeks (SAT) | Certificate: 0 boards directly, but sorts the 18 into "worth compute" and "infeasible given the fixed surroundings". SAT: 10-25 % | Luo and Wong [V] (10 x 10 pins in ten minutes, 2008); Nam et al. [V] warn that fixed boundaries create false "unroutable" |
| 5 | **Joint via-site assignment and a via-aware layer assignment** (F2 of doc 16, with the crossing graph as a constraint, not the objective) | 2-3 weeks | 5-10 % | Package-substrate literature [V/A]; tscircuit's own joint plane-site fix [V, first note T7]. Weak for outer-ring-with-far-partner failures |
| 6 | **Shove / compaction inside a transaction** (gridless push of committed tracks to open a channel; F6 of doc 16) | 4-8 weeks | 5-15 %, high variance | KiCad PNS and Specctra-style routers [A]; the lattice-waste number below [C]; contradicted by the pitch experiments (Y1, E3) |
| 7 | **Topological or ordered-escape planner** (F1 of doc 16) | 6-10 weeks | About 5 % of the last few; may matter more for the 28 big boards, unproven | Strong literature for ordered escape between two dense components; E1 (tscircuit topology as guide) was negative |
| 8 | Cost-scheme variants: lexicographic "fewest victim nets first" objective, net-level aging, multiplicative-weights averaging | 1-3 days each | 3 % or less | Weak; the repo already measured four cost knobs with no effect |
| 9 | Layer assignment by the crossing graph (bipartite / max-cut / via minimisation) | done (X2) | At or below 0 (measured: 18 % to 14 % clean on the srj20 gap set) | Classical theory [A]; objective is wrong for escape areas (section 3) |
| 10 | CBS, PBS, PIBT, LaCAM (MAPF solvers) | high | 0 | PBS and Greedy CBS were worse than plain prioritised planning even as the repair step of MAPF-LNS2 [V]; PIBT's guarantee is for moving agents on biconnected graphs [V] |
| 11 | Hexagonal, 60-degree (Y), X or any-angle lattices | very high | 0 on a square ball grid | Y-architecture is 2.5 % below X on throughput and 4.3 % longer in wire [V]; no 45-degree or 60-degree corridors exist between square-grid pads at 0.8 mm [C] |

**Do first, before any of it (1-2 days, no router change): classify the 18 boards.** For each open connection on a frozen
best state, with every unfixed track within R = 2, 4, 8 mm removed: is the ball reachable, is the partner pad
reachable, which nets form the minimum blocking set. If many are sealed by fixed copper or by the pads of the two
ends, stop: the exact candidates cannot help and the answer is a certificate (rank 4) and a rule change (rank 2).

## 1. What the plateau probably is, and which candidate attacks which cause

Three explanations are consistent with what the repo measured; the candidates attack different ones.

1. **Combinatorial.** A complete assignment of routes exists, but the search cannot generate it. Sequential A* gives each
   connection its shortest path given the others, so a solution that needs an earlier connection on a longer
   detour (to free a channel) is out of reach for ordering experiments: the group re-route of doc 05 §17 tried all
   orders of up to five connections, and every order yields only greedy-consistent combinations. Annealed
   transactions keep wandering among nearly complete states (doc 16 §12), which is what a combinatorial plateau looks like.
   Attacked by ranks 1, 3, 4, 5.
2. **Geometric.** Lattice quantisation wastes the last slice of room. Computed for the srj fixture (0.15 mm track + 0.10 mm clearance,
   so 0.25 mm between parallel centre-lines) [C]: at 0.04 mm an orthogonal parallel pair needs 7 cells = 0.28 mm
   (12 % waste), a diagonal pair needs 9 cells = 0.2546 mm (1.8 %); at 0.05 mm the orthogonal case is exact and the
   diagonal costs 13 %. The repo's own note (`dev/m15-notes.md`) uses 0.22 mm and finds 11 %. This is real but
   small, and the experiments that tried to exploit it (pitch choice, 20 µm lattice) were neutral or worse at equal
   work. Attacked by rank 6 only.
3. **Infeasible** under the rules, given where the earlier connections are or even in absolute terms. tscircuit's router
   solves some boards this one does not, so not all are infeasible in absolute terms, but "infeasible given a fixed
   global structure" is a different statement and is what an exact solver returns (next paragraph).

A caution from the one source that tested it: Nam, Aloul, Sakallah and Rutenbar (ISPD 2001) ran a Boolean detailed router on
FPGA circuits with the global routing fixed. For 4 of the 5 circuits singled out in their Table 5, the SAT router proved
the layout unroutable at a track count where VPR routed it, because VPR also changes its global routing. When VPR's own
final routes were handed to the SAT formulation as the global routing, it reproduced VPR's results on all circuits but
one (k2, 9 tracks), where it found no solution in 24 hours [V]. Two lessons: an exact solver on a window whose
surroundings are frozen can answer "infeasible" for a board that is routable, so the window must free whole nets (all
of their copper) and not cut routes at its boundary; and exact solving has instances it simply does not finish, so every
call needs a work limit (rule 5).

## 2. Topic 1: hexagonal, 60-degree, Y, X and any-angle routing

**What the sources claim.**

| Claim | Source | Tag |
|---|---|---|
| Y architecture (0, 60, 120 degrees) throughput +30.7 % over Manhattan on a square mesh under uniform demand; only 2.5 % below X architecture; a 2-pin net is 13.4 % shorter than Manhattan; average wire only 4.3 % longer than X | Chen, Yao, Zhou, Cheng, ASP-DAC 2003 | [V] abstract |
| With a Rentian demand model the Y architecture gains about 20 % over Manhattan on a square chip, "very close" to X | Chen, Cheng, Kahng, Mandoiu, Wang, Yao, ICCAD 2003 / TCAD 2005 | [V] |
| X architecture: more than 20 % less wire and 30 % fewer vias than Manhattan | Teig, SLIP 2002 | [A], author's own projection, baseline not checked |
| Y-architecture routing of **hexagonal flip-chip pad arrays**: triangular tile model, chord-based node division, dynamic-programming bend minimisation, 100 % routability | Nie, Chen, Chang, DAC 2022 | [A] abstract |
| Hexagonal or face-centered **ball patterns** give "8-10 % more escape routes than square patterns" | Brist (Intel), US 9,872,397 B2, filed 2012 | [A], a patent's own statement |
| Network-flow escape on staggered / hexagonal arrays with correct capacities, optimal | Wu and Wong, ASP-DAC 2013 | [A] |
| Any-angle routing for RDL in 2.5D packages beats Manhattan RDL routers in wirelength; a 2024 NTU thesis that keeps 90/135-degree turns but inserts vias dynamically is reported to match or beat an any-angle router | Chung et al., DAC 2023 (via the NTU thesis page) | [A], one secondary source |

**Judgement: do not build.** The reasons are geometric, not statistical.

- All of the above measure wirelength or throughput of free routing, and the X architecture, which the router
  already is (octilinear), is **ahead** of Y on both.
- The gain claimed for hexagonal arrays belongs to the ball pattern, not the router. The ball grid on these boards
  is square and fixed.
- On a square ball grid the only straight corridors between pads run along the grid axes. For a direction (a, b) the
  free width between adjacent lattice lines is (p - 2h(|a|+|b|)) / sqrt(a² + b²) for square pads of half-side h [C].
  At pitch p = 0.8 mm with 0.36 mm pads (the srj geometry): axis 0.440 mm (one track needs 0.35); 45 degrees
  **0.057 mm** (0.206 mm with round pads); every other lattice direction is negative (slope 1:2: -0.125 mm); 60 degrees is not a lattice direction at all. Neither a
  60-degree nor a 45-degree track can run through the array. Inside the array the router only needs short
  diagonal pieces to turn in an interstice, and the octilinear lattice has them. A hexagonal lattice would change
  nothing there and would cost a new clearance model, new GPU fields and a new DRC path.
- In the annulus outside the array the question is capacity of parallel bundles and crossings, not diagonal
  direction count; the 12 % lattice waste above is the only thing a different lattice could recover, and a
  lattice of the right pitch recovers it more cheaply (measured neutral).

Where it would matter: real packages with staggered or hexagonal ball patterns (flip-chip pad arrays in the cited
papers). Then the escape-capacity model (not the routing lattice) needs the staggered-array capacities of Wu and Wong.
That is a planner input, relevant only if F1 of doc 16 is ever built.

## 3. Topic 3: layer assignment as a graph, and where the vias sit

**Theory.** With vias allowed only at terminals, two-layer routability of a fixed topology is 2-colourability of the
crossing (conflict) graph: a bipartite graph is assignable, an odd cycle is not. With vias allowed in the interior the
problem becomes via minimisation:

- The **unconstrained** form (move vias and wires freely) has a polynomial algorithm for planar-structured instances
  through planar maximum cut (Hadlock, SIAM J. Comput. 4(3), 1975) [A]; Chen, Kajitani and Chan (IEEE Trans. Circuits Syst., 1983, "A graph-theoretic via minimization algorithm for two-layer
  printed circuit boards") is the PCB-specific graph formulation; I recalled the title and could not find the paper or its
  abstract, so it is unverified [A]; Marek-Sadowska, IEEE TCAD 3(3), 1984 [A].
- The **constrained** form (routing fixed, choose which segments get vias) is NP-complete (Naclerio, Masuda, Nakajima,
  Univ. of Maryland ISR TR 1987-42) [A]. Marek-Sadowska's earlier hardness claim for a variant had a flaw that
  Rim, Kashiwabara and Nakajima corrected in 1989 [A]: treat the 1984 hardness statement with that caveat.
- On bus escape, **optimal** layer assignment exists for buses with the longest-common-interval formulation
  (Kong, Yan, Wong, Ozdal, ICCAD 2007, O(n log n), optimal for one layer [A]; Yan, Kong, Wong, optimal layer assignment
  for bus escape [A]). Ozdal and Wong (TCAD 2006) take the maximal planar subset per layer by an exact algorithm for
  "longest path with forbidden pairs", then distribute the rest; the 2005 follow-up reduces vias by 39 % on industrial
  cases [A].

**Why this has not helped, and will not.** The objective in all of these is fewer vias or fewer crossings. In an
escape area at 0.8 mm the scarce thing is the **site** of a via, not the number of them [C]:

- A through via with its clearance needs 0.30 + 2 x 0.10 = 0.50 mm; the channel between two pads is 0.44 mm. A via in
  an interstice therefore closes the row channel and the column channel that meet at it, on the layer that carries the
  tracks and (as a copper land) on the other.
- A full N x N array has N² balls and only (N-1)² interstices (100 against 81 for N = 10). A "dog-bone via per ball"
  is impossible for a full array at this pitch; the srj boards escape because they are sparse (40-110 connections).
- The first note's L6 says the same from the package-substrate literature: via assignment has to be decided with the
  routing. In 2-layer BGA **package** routing (not PCB), Kubo and Takahashi (IEICE 2005) fix each net's global route by
  its via assignment and improve the assignment iteratively [A]; Tomioka, Kurata, Kohira and Takahashi (IEICE E92-A,
  2009) formulate it as a MILP with a restricted route structure and place the via near the ball; CPLEX 11, seven data
  sets, "within a practical time" [V]. Both show that decisions on via sites belong inside the plan.

The repo's own X2 (max-cut on the straight chords: 18 % to 14 % clean) fits this: it optimises the wrong quantity.
A usable version would keep the crossing graph as a **constraint** (two crossing chords must differ in layer or one
must take a via site) inside the joint via-site matching of F2 (rank 5), not as a stand-alone preprocessing.

**Does dog-bone versus via-in-pad change feasibility? Yes, structurally.**

- Via in pad removes the interstice via and its stub from the pad layer. The row and column channels stay continuous
  on the pad layer and every ball changes layer without costing any channel there. On the other layer the land sits at
  the ball position, 0.30 mm wide in a 0.80 mm cell, so the channel next to it is 0.50 mm [C].
- Vendor sources agree on the direction and disagree on the cut-off: dog-bone is "possible" at 1.0 mm; at 0.8 mm
  opinions differ; at 0.5 mm sources say dog-bone is not possible under standard fab capability and via-in-pad or
  micro-vias are needed [A, trade articles; Altium, JLCPCB, PCD&F]. These are planning estimates, not IPC limits.
- The cost is real (filled and capped vias, a fab option, impedance concerns on high-speed nets [A]); the first note's
  L11 already says it must be an explicit rule-gated capability. tscircuit's own sets forbid it, so it cannot move
  srj scores and can only be measured on KiCad-judged boards.

**Rank 2 candidate: via-in-pad rung.** One option (`--via-in-pad`, default off) allows a through via whose centre lies
inside a pad of its own net on a ball-grid package, tried only as the last escalation rung for an open connection
whose ball is boxed in or whose escape channels are all taken. DRC: as far as I know KiCad raises no clearance error for
copper of the same net overlapping, but this was not tested here; the exact check (rule 1) stays the gate. Effort 3-5 days (via legality predicate, pad-escape move set, a flag in the rule
reader, tests, a report line saying how many vias sit in pads). **Estimate [J]:** unknown. For the plateau class
(outer-ring ball with a far partner) a ball on the outer ring usually has a free outward channel, so the benefit comes
from the interstices that its neighbours no longer need for dog-bone vias, which is indirect. Measure before believing:
run the 76 boards that one variant leaves open with the option on; **drop** it if it closes fewer than a quarter of the
1-3-open boards. Report it as a separate row, never inside a tscircuit comparison.

## 4. Topic 2: topological routing, rubber band and ordered escape

**What exists.**

| Work | Core | Fit to this problem | Tag |
|---|---|---|---|
| Dai, Dayan, Staepelaere, "Topological routing in SURF: generating a rubber-band sketch", DAC 1991; Dai, Kong, Sato, "Routability of a rubber-band sketch", DAC 1991 | Wires are elastic bands that keep their homotopy; a sketch is tested for routability, then converted to geometry (Staepelaere, UCSC-CRL-92-39, converts to rectilinear or octilinear wiring of minimum length) | The sketch-to-geometry step is the analogue of the gap E1 (doc 16) exposed: a topology is only as good as the geometry step that realises it. I found no published routability numbers for dense BGA | [A] |
| Dayan and Dai, "Layer assignment for rubber band routing", UCSC-CRL-93-04 | Steepest-descent layer assignment on a rubber-band sketch with a wirelength-and-via cost; "Slice" (a router the report cites) peels a maximal planar subset per layer | Same planar-subset idea as Ozdal and Wong | [V] abstract |
| Dayan, PhD thesis 1997, implemented as gEDA/PCB `toporouter` (Anthony Blake, 2008); TopoR (Eremex, Delaunay-triangulated topological autorouter, first Windows version 2001) | Topological path first, geometry afterwards | The descendants I found; the gEDA code is described as experimental and sometimes producing invalid routings; TopoR's BGA performance is not documented in anything I could read | [A] |
| Seong, Yang, Han (Samsung SDS), "Topology for substrate routing in semiconductor package design", arXiv 2105.07892, 2021 | Circular-frame topological transformation for package substrates | The abstract says it "competes with certain grid-based routing algorithms"; no code and no BGA numbers found | [V] abstract, weak |
| Tan Yan and Wong, "A correct network flow model for escape routing", DAC 2009 | Optimal unordered escape with correct diagonal capacity and missing pins | Already used (`route/escape_flow.cpp`); fixes capacities, not order | [A] |
| Luo and Wong, "Ordered escape routing based on Boolean satisfiability", ASP-DAC 2008 | Exact SAT on a grid: node variables carry the net index in binary, edge variables; MiniSat; 1-4-side escape; adds tracks where the solver's conflict clause points | The only exact ordered-escape method found. Pure SAT handled "approximately 10 x 10 pins" within ten minutes on a Pentium 4 (2008); larger arrays were partitioned. One track per channel except where conflicts add more | [V] |
| Ozdal and Wong, TCAD 2006; Ozdal, Wong, Honsinger, ICCAD 2005 | Escape from several components at once so that crossings in the intermediate area are minimal; planar subset per layer | Needs two dense components with boundary orders. Here the far ends are single scattered pads | [A] |
| Sattar and Ignjatovic, MJCS 2016; Wang, Shi, Cheng, ICCAD 2006 | Flow models for order plus escape; central triangular pin sequence minimises layers (a 35 x 35 array breaks out in 3 layers where the ring-by-ring order needs 6) [V] | The triangular sequence is for full arrays and several layers | [V] abstract of the second |
| ISPD 2022 panel on PCB routing (Yih-Lang Li) | Lists SAT and ILP as the main ordered-escape methods and says there is no benchmark except the DAC 2021 industry cases | Confirms there is no public score to compare against | [V] |

**Judgement.** The ordered-escape literature solves "order the boundary of component A to agree with component B",
exactly or heuristically, for a handful of well-formed instances. The failing connections here have **destinations
scattered over the board**, so the intermediate area is the whole board and the useful order is the angular order of
the partners around the package. That is a chord diagram on a circle: connections cross if their (exit angle,
destination angle) pairs interleave. The repo's max-cut layer assignment (X2) already used this crossing structure with
straight chords and gained nothing. What the literature adds is the **exact** treatment, and its limit: Luo and Wong
solve 100 pins in ten minutes (2008 hardware, MiniSat), the mixed-integer packaging methods run on seven data sets. No
source reports completing a two-layer BGA board with scattered destinations.

Effort for a planner (F1): 6-10 weeks. Do not build it on this evidence. The only path I can defend: use Luo and Wong's
SAT formulation not as a router but as the local oracle of rank 4, on the 8 x 8 ring around the open ball, with
free exit "slots" and an angular-order requirement derived from the partners.

## 5. Topic 4: exact and complete methods for the final connections

### 5.1 Route-pool exact selection (rank 1)

**Core idea.** Instead of re-deriving a route each time a transaction rolls back, keep every full route the search
has produced for the open connection and for the nets near it (the pool). Pick the largest subset that is mutually
compatible, exactly. Two routes of different nets conflict if the exact clearance predicate says so; each connection
picks at most one of its routes; maximise the number of connections routed. This is a maximum independent set on a
conflict graph, or SAT with "at most one route per net" and one clause per conflicting pair or per conflicting
clique. When the exact answer is "one connection short", the solver's conflict (the clause set that makes it
unsatisfiable) names the nets whose routes block each other, and those nets get new candidate routes (A* with the
conflicting cells penalised or forbidden), as in column generation.

**Evidence.**

- Nam, Aloul, Sakallah, Rutenbar, ISPD 2001: a formulation with one Boolean variable per **candidate detailed
  route** instead of one per track was 66x faster on average than the track-variable formulation, and the Boolean
  router can prove unroutability, which net-at-a-time routers cannot [V].
- Jiang, Khatri, Sangiovanni-Vincentelli, Brayton, UCB/ERL M99/16 (1999): nets are clustered, each cluster's routes
  chosen from predetermined 2-4-via patterns by SAT; "if the net cluster size is less than 25, the SAT router is able
  to efficiently find the routing solution"; total length on average 1.47 % above Manhattan [V]. Old hardware and
  pattern-based, but it is a direct data point for the size of a cluster.
- Qi, Hu, Peng, You, Han, Zhu (Xidian), ASP-DAC 2024 (slides): "net routes ripped up are discarded too early in
  sequential routing"; the paths produced during rip-up and reroute are **collected and selected by an integer
  program** (CPLEX) inside TritonRoute-WXL on the ISPD-2018 suite; fewer violations per iteration, fewer iterations, less
  runtime, IP solving "a fraction of runtime". Slides only: the numbers are in figures I could not read [A, slides].
  This is the same move as proposed here and the strongest recent precedent.
- Lam, Le Bodic, Harabor, Stuckey, "Branch-and-cut-and-price for multi-agent pathfinding" (IJCAI 2019; journal
  version): exact, path variables priced by shortest-path search, thirteen classes of conflict constraints; solves 2,402
  of 4,430 instances against 2,039 (Lazy CBS) and 1,939 (CBSH2-RTC) [V abstract]. It shows that the path-variable master
  problem with a shortest-path pricing step, which is what A* with prices already is, beats conflict-tree search on
  tight grids. Agents, not copper, and time-expanded: transfer is by analogy.
- The same slides criticise lattice-wide ILP routers (Han, Kahng, Lee, DAC 2015; Jia, Cai, Zhou, Yu, TCAD 2018, a
  multicommodity-flow detailed router): "a large number of grids need to be solved", complex design rules are hard to express,
  off-grid paths are hard to generate, high quality but long runtime [A, slides]. That is the argument for a pool of A*
  routes with an exact selection, rather than an ILP over every lattice node.
- Fang, Hsu, Chang, TCAD 2009 (flip-chip RDL) is an ILP router that is exact and slow; a 2012 successor reports 125x
  speed-up with the same routability [A]. The usual price of exact methods; keep to windows.

**Why it should beat what was tried.** The group re-route of doc 05 §17 and the victim-order experiments Y2/Y3 vary
the **order** of greedy A* calls; every result is a route set the greedy structure can produce. A pool with diversified
routes (a detour that frees a channel is in the pool, even though no shortest-first order would pick it) contains
solutions that no order reaches, and the exact selection finds them if they exist [J: reasoning from how greedy search
works, not a measurement].

**Design for this router.**

1. Pool generation reuses `search()` with: (a) the usual negotiated cost; (b) cells of the current routes of the
   neighbours given a growing penalty; (c) seeded cost jitter (counter-based, rule 2); (d) forbidding a chosen edge set
   from the previous best route. 8-32 routes per connection; neighbourhood = the open connections plus all nets with
   copper within R of their pads (R chosen by the classification of section 0).
2. Conflict test between two routes: bounding-box reject, then the existing exact clearance predicate on the
   two polylines/vias (rule 1 stays the only judge). Cache by route id.
3. Selection: at most 30-60 connections x 8-32 routes, so up to 2,000 vertices. Own branch and bound with a
   clique bound (the precedent is D23, where the exact placement windows are an own branch and bound over candidate
   slots with a full-enumeration reference path) or a vendored small CDCL solver (MiniSat or CaDiCaL are permissively
   licensed; OR-Tools is not installed here, see D23). Reference path: brute force over all subsets for at most 6 nets;
   equivalence test as rule 3 requires.
4. Commit through a transaction (rule 4): the new copper replaces the old only if the exact check passes and the
   routed count rises.
5. Feed back: if the best subset leaves one connection out, take the solver's conflict set, add penalised routes for
   those nets, resolve. Stop on work units.

**Effort.** Pool generator 3-4 days; conflict test and cache 2-3 days; solver 3-5 days (own B&B), plus the reference
test; transaction wiring 2 days; tuning and the determinism test 3-4 days. About 2-3 weeks. A prototype outside the
router is cheaper: dump the pool (routes as polylines) from the 18 boards with a debug option, then solve in Python
with `scipy.optimize.milp` (HiGHS; scipy 1.18 is installed here). That costs about 3 days and answers whether the
idea works before any C++.

**Estimate [J].** If the 18 near-complete boards are mostly "a solution exists in the neighbourhood", the idea
closes a quarter to two-fifths of them: +3 to +8 boards of 200 on srj20, nothing on the 28 big boards (8-40 open
cannot be fixed by a 30-connection window). **Decision rule:** the offline prototype must complete at least 4 of the 18
boards (22 %), otherwise stop. Risk: pool size needed grows fast; the classification of section 0 tells whether the
open connections are sealed.

### 5.2 Exact lattice window, used as an oracle and a certificate (rank 4)

**Core idea.** For a window around the open connection, with whole nets freed (not cut at the boundary), build the
Boolean model on the lattice itself: node variables with a binary net index (Luo and Wong), edge variables, clearance as
pairwise exclusion within the track-plus-clearance radius, via nodes closing both layers. Solve with a CDCL solver. SAT
gives a route set; UNSAT gives a **proof** that this window, with these nets, cannot be completed, and the conflict
clause points to where.

**Evidence.** Luo and Wong (above) [V]: 10 x 10 pins in about ten minutes on 2008 hardware, partitioning beyond that;
Nam, Sakallah and Rutenbar, "Satisfiability-based layout revisited: detailed routing of complex FPGAs via search-based
Boolean SAT", FPGA 1999: a search-based SAT router (GRASP) for whole FPGAs, "the first evidence that SAT methods can
actually demonstrate the unroutability of a layout" [V abstract]; Nam et al. ISPD 2001 (above) [V], including the warning
in section 1. A grid of 125 x 125 x 2 layers for a 5 x 5 mm window
at 0.04 mm has 31,000 nodes and, with 15 nets, several hundred thousand variables before clearance clauses; I found no
modern timing for this size [J: seconds to minutes on a current CDCL solver, to be measured].

**Judgement.** Doc 05 §20 and doc 16 F5 declined an exact window because it "would mostly return 'feasible, the
heuristic just did not find it'". In the present regime that answer is the one that completes the board. The
real objection is effort and the false-infeasible risk. Use it as: (1) a ground-truth oracle for deciding whether
rank 1 and rank 3 are leaving solutions on the table (run on 20 windows of three nets), and (2) a certificate that retires
boards from further compute. **Effort 3-5 weeks**, mostly the clearance encoding. **Estimate [J]:** 10-25 % of 1-3-open boards
closed directly. Not before the route-pool prototype has been tried, since that reaches the same solutions with
the existing search and the existing exact clearance predicate.

**The cheaper certificate, built first (1-2 weeks).** Doc 16's E5/F5: an LP relaxation of the multi-commodity flow on the
window lattice, or a per-layer cut count around the package with the exact gap capacities of `escape_flow`. Fractional
infeasible implies infeasible; the converse does not hold. The planar theory is of little help: the Okamura-Seymour
theorem makes the cut condition sufficient when all terminals lie on one face of a planar graph (and on two faces in
Okamura's extension) [A, arXiv 2007.01280 states it]; two layers with vias are not planar and the partner pads are on
many faces, so only necessity can be used.

### 5.3 LNS with spatial and adaptive neighbourhoods (rank 3)

**Core idea.** Treat the transaction machinery as a large-neighbourhood search whose neighbourhood is chosen
deliberately. Neighbourhood types: (a) the victims of the cheapest path (what runs today); (b) all nets with copper
within R of the open ball or the open partner pad; (c) a random walk along conflicting routes, as in MAPF-LNS2's
"collision-based" neighbourhood; (d) "failure-based": the nets sitting on the target side of the open connection.
Re-route the neighbourhood in a random priority order, accept if the number of open connections does not grow, adapt the
weights of the neighbourhood types by their recent success.

**Evidence.**

- Li, Chen, Harabor, Stuckey, Koenig, AAAI 2022 [V]: repair from a colliding plan by replanning a chosen subset in
  random priority order, keep the change iff the number of colliding pairs does not increase; three neighbourhood
  types and an adaptive weighting by the reduction they achieved; neighbourhood sizes 4, 8, 16, 32 compared ("larger
  neighbourhoods have larger chances to find better solutions but require more time"); solves 80 % of the largest
  random-map instances in five minutes against prioritised planning with restarts, EECBS and PPS. The repo's
  transactions took over the acceptance rule only; the types (b)-(d) and the adaptivity were not taken.
- A footnote there says the authors tried Greedy CBS and PBS as the repair step and **both were worse than plain
  prioritised planning** [V]. That is the best available evidence against putting conflict-tree search in the loop.
- Tan, Luo, Li, Ma, "Reevaluation of large neighborhood search for MAPF" (arXiv 2407.09451): rule-based neighbourhood
  heuristics are strong baselines; learned selectors show no clear advantage [V abstract]. Supports keeping it
  rule-based and cheap.
- The repo's `--lns-open` (lift the neighbours of an open connection, route it first, re-route them in seeded orders) was
  "never reached at 50 M" (doc 16 §10): the passes used the whole budget before it ran. It was never measured, so this is
  a missing experiment, not a refuted one.

**Effort.** 3-5 days on top of `txn_try`/`txn_capture`/`txn_restore`: neighbourhood extraction (spatial index exists),
a second phase that starts from a saved best state (`--state-out` / `--state-in`, doc 05 §41.1) so the main phase is not
shortened (`--polish` reserved a share of the budget and "loses more in the shortened main phase"), adaptive weights with a
counter-based generator.

**Estimate [J].** 10-20 % of 1-3-open boards; the main value is that it supplies the neighbourhoods and the pool dumps that
rank 1 needs. **Decision rule:** at equal work, fewer than +3 boards of the 18, or any lost board, drops it.

### 5.4 Shove and compaction (rank 6)

Maze routers with shove (Specctra, Freerouting, KiCad PNS): shoving moves committed traces aside inside their
clearance slack to open space; walkaround avoids them; KiCad's router has shove-only, walkaround-only and a smart mode
that picks the less disruptive, and three optimiser effort levels [A, Doxygen of `pns_*`]. Freerouting works on a
continuous plane of convex "expansion rooms" with A* over rooms and doors, per a third-party reading of the source
[A, weak]; I found no Freerouting or PNS publication with completion numbers on dense BGA fan-out, only forum
anecdotes that autorouters do worse than a human on a two-layer BGA [A]. Classical rip-up routers add penalty-driven
iteration (Nair 1987; Linsker's VIKING, IBM J. Res. Dev. 28(5), 1984) [A], the ancestors of what the repo already does.

Applicability here: one track fits a 0.44 mm channel, so shove cannot create capacity inside the array; in the annulus
it recovers the 12 % lattice waste computed in section 1 and the room that a lattice route leaves beside a bend.
That is at most a track's worth of room per bundle, which is what the last connection often needs, but E3 and Y1 show no
benefit from changing the lattice, which argues against slack being the limit. **Effort 4-8 weeks** (a continuous
solver with the exact check as a gate, topology-preserving, deterministic). **Estimate [J]:** 5-15 %, high variance.
Start it only if the classification of section 0 finds open connections that route when the lattice is locally halved (the
existing half-pitch last rung): that is the signature of a slack-limited failure.

### 5.5 Cost schemes (rank 8)

The repo has varied crossing cost, history weight, growth of the crossing cost, victims per route, and annealing. Not
tried, cheap, and justified only by analogy:

- **Lexicographic objective.** MAPF-LNS2's single-agent search minimises the *number of collisions first*, then length,
  with no weights (SIPPS, [V]). The repo's search adds a crossing cost per lattice step; a long overlap with one
  victim costs many steps while the transaction only cares how many nets it displaces. A variant that counts each
  victim net once (state: set of victims met, feasible only for small sets) is not Markovian and approximate in
  practice. 2-3 days. Expected gain 3 % or less [J].
- **Net-level aging.** Stuck connections get a priority that rises with each failure so that they win ties for
  contested sites (tscircuit's fix T8 for buses promoted blocked buses). The repo has per-connection rip counts but
  uses them to forbid, not to prioritise. 1 day. 3 % or less [J].
- **Multiplicative-weights averaging** over the history of routes (the resource-sharing view behind BonnRoute,
  Gester et al., TODAES 18(2), 2013 [A]) replaces the last history with an average; fractional, then rounded. Needs the
  rounding step that rank 1 provides anyway. Do not build separately.

NTHU-Route 2.0 (Chang, Lee, Wang, ICCAD 2008), the winner of the ISPD 2008 global routing contest, uses a history-based cost and
new net orderings, and it still routed only 12 of 16 ISPD-08 benchmarks without overflow [A]: cost schemes shrink the
plateau, they do not remove it. CUGR (Liu et al., DAC 2020), FastRoute and Dr. CU / Dr. CU 2.0 (Li, Chen, Jiang, Chen,
Young, ICCAD 2019) add pattern routing, layer-aware maze search and sparse data structures; I found nothing in their abstracts that
targets the last blocked nets beyond ordering, history cost and bigger windows [A]. The one relevant new element in
this family is the IP selection of candidate routes (IPGA, above), which is rank 1.

### 5.6 MAPF solvers (rank 10)

- Conflict-based search (Sharon et al., AIJ 2015) and priority-based search (Ma et al., AAAI 2019) branch on
  vertex-time conflicts. With no time axis the branching is on "net A may not use this cell", an exponential tree
  in the number of conflicts, and on dense grids BCP (above) beats CBS. LNS2's footnote shows them worse than PP as
  repair engines [V].
- PIBT (Okumura et al., IJCAI 2019 / AIJ 2022): agents move one step per tick with priority inheritance, backtracking
  guarantees every agent reaches its goal in finite time on graphs where every adjacent pair lies on a simple cycle
  [V]. The guarantee is about moving agents and finite waiting. A routed copper track does not move; nothing transfers.
- LaCAM (Okumura, AAAI 2023): complete search-based MAPF that detects unsolvable instances [A]. Time-expanded, same
  objection.

What does transfer from MAPF is the LNS family (rank 3) and the path-variable exact master problem (rank 1).

## 6. Ordering the work

1. **Classification** of the 18 near-complete boards (1-2 days). Output: for every open connection, sealed-at-ball,
   sealed-at-partner, or blocked mid-route; the minimum blocking sets; for the sealed ones, whether a via-in-pad would
   open the ball. Gate: if more than half are sealed by fixed copper or by pad geometry, go to ranks 2 and 4
   (rule change, certificate) and stop the search-side work.
2. **Rank 3** (3-5 days). Build the neighbourhood extractor and the pool dump. Gate: +3 boards of the 18 at equal work.
3. **Rank 1 prototype offline** in Python/HiGHS on the dumped pools (3 days). Gate: 4 of 18. Only then C++.
4. **Rank 2** in parallel as a policy option, measured on KiCad-judged boards only.
5. Ranks 5, 6, 7 only if ranks 1-3 leave a documented residue of boards that the oracle (rank 4) shows to be feasible.

Things outside routing that change feasibility and that the repo already has a channel for (doc 04 §4 placement ECO):
a third copper layer, a few tenths of a millimetre of package or passive movement, pin/gate swapping where the netlist
allows it. They are not search improvements and are listed so that the "infeasible" label is not mistaken for "unfixable".

## 7. Where the sources are weak

- **Everything numerical in section 5 and the ranking is judgement [J].** No source reports a BGA lattice router
  completing the last connections by any of these methods. The literature is on FPGA routing (Nam et al.), multichip modules
  (Jiang), package substrates (Kubo, Tomioka), flip-chip RDL (Fang, Lee), and MAPF.
- The Xidian IPGA paper (ASP-DAC 2024) was read as slides; its quantitative table is in figures. The claim that
  it reduces violations and runtime against TritonRoute-WXL is the authors', not checked.
- Y-architecture figures come from the authors' own multi-commodity-flow model on meshes (Rentian or uniform demand). Teig's
  20 %/30 % is a projection. None is a PCB measurement.
- The Intel hexagonal ball-pattern "8-10 %" is a patent statement. The vendor via-in-pad thresholds (0.5, 0.65, 0.8
  mm) contradict each other and are not IPC limits.
- TopoR and the gEDA toporouter: no BGA numbers anywhere; a developer's page (ssalewski.de) calls the toporouter experimental. The
  Seong et al. substrate paper has no numbers in its abstract and no code.
- Tomioka et al. and Kubo and Takahashi are **package substrate** routers (finger to ball, one chip side), not PCB
  breakouts; the via-assignment structure transfers, the problem does not.
- "Kong, Ho, Fang" as named in the question: I read them as Hui Kong (bus sequencing, ICCAD 2007, with Yan, Wong, Ozdal),
  Jia-Wei Fang (flip-chip RDL, with Y.-W. Chang) and, a guess, Y.-K. Ho (a DAC 2012 flip-chip paper). If other papers
  were meant, they were not found. None of these has a two-layer PCB BGA with scattered destinations as its subject. "Yan and Wong 2009" is by **Tan Yan** and
  M. D. F. Wong; the first note writes "T. Yan".
- Not retrieved: NTHU-Route 2.0 and MCFRoute (Jia et al., TCAD 2018) full texts (the hosts timed out from here), the
  Dr. CU 2.0 text, the Frankfurt "exact algorithm for difficult detailed routing problems" paper (access denied), Shi and
  Cheng's DAC 2006 hexagonal escape paper (not found), and any independent measurement of tscircuit's force-improve stage.

## Sources

Hexagonal, Y, X, any-angle

- Chen, Yao, Zhou, Cheng, "The Y-architecture: yet another on-chip interconnect solution", ASP-DAC 2003 [V]:
  <https://cecs.uci.edu/~papers/compendium94-03/papers/2003/aspdac03/pdffiles/09b_3.pdf>
- Chen, Cheng, Kahng, Mandoiu, Wang, Yao, "The Y architecture for on-chip interconnect: analysis and methodology", ICCAD 2003 / IEEE TCAD 2005 [V]:
  <https://cseweb.ucsd.edu/~kuan/yarch/iccad.pdf>
- Teig, "The X architecture: not your father's diagonal wiring", SLIP 2002 [A]: <https://www.springerprofessional.de/doi/10.1145/505348.505355>
- Nie, Chen, Chang, "Y-architecture-based flip-chip routing with dynamic programming-based bend minimization", DAC 2022 [A]:
  <https://doi.org/10.1145/3489517.3530577>
- Wu and Wong, "Network flow modeling for escape routing on staggered pin arrays", ASP-DAC 2013 [A]:
  <https://scholars.hkbu.edu.hk/en/publications/network-flow-modeling-for-escape-routing-on-staggered-pin-arrays/>
- Brist, "Symmetrical hexagonal-based ball grid array pattern", US 9,872,397 B2 [A]: <https://patents.google.com/patent/US9872397B2/en>
- Any-angle RDL (Chung et al., DAC 2023, via an NTU thesis record) [A]: <https://tdr.lib.ntu.edu.tw/handle/123456789/87474>

Escape routing, topological routing, layer assignment

- Luo and Wong, "Ordered escape routing based on Boolean satisfiability", ASP-DAC 2008 [V]: <https://www.cecs.uci.edu/~papers/aspdac08/pdf/p244_3A-4.pdf>
- Tan Yan and Wong, "A correct network flow model for escape routing", DAC 2009 [A]:
  <https://scholars.hkbu.edu.hk/en/publications/a-correct-network-flow-model-for-escape-routing/>
- Ozdal and Wong, "Algorithms for simultaneous escape routing and layer assignment of dense PCBs", IEEE TCAD 25(8), 2006 [A]:
  <https://scholars.hkbu.edu.hk/en/publications/algorithms-for-simultaneous-escape-routing-and-layer-assignment-o/>
- Ozdal, Wong, Honsinger, "An escape routing framework for dense boards with high-speed design constraints", ICCAD 2005 (via count -39 %) [A]:
  <https://scholars.hkbu.edu.hk/en/publications/an-escape-routing-framework-for-dense-boards-with-high-speed-desi/>
- Kong, Yan, Wong, Ozdal, "Optimal bus sequencing for escape routing in dense PCBs", ICCAD 2007 [A]:
  <https://scholars.hkbu.edu.hk/en/publications/optimal-bus-sequencing-for-escape-routing-in-dense-pcbs/>
- Wang, Shi, Cheng, "Layer minimization of escape routing in area array packaging", ICCAD 2006 [V abstract]: <https://www.cecs.uci.edu/~papers/iccad06/papers/10C_3.pdf>
- Kubo and Takahashi, "A via assignment and global routing method for 2-layer ball grid array packages", IEICE E88-A(5), 2005 [A]:
  <https://globals.ieice.org/en_transactions/fundamentals/10.1093/ietfec/e88-a.5.1283/_p>
- Tomioka, Kurata, Kohira, Takahashi, "MILP-based efficient routing method with restricted route structure for 2-layer ball grid array packages", IEICE E92-A(12), 2009 [V]:
  <https://u-aizu.ac.jp/~ytomioka/papers/2009_IEICE_Trans_Tomioka_2.pdf>
- Sattar and Ignjatovic, "Simultaneous escape routing using network flow optimization", MJCS 29(2), 2016 [A]: <https://ejournal.um.edu.my/index.php/MJCS/article/view/7006>
- Fang, Hsu, Chang, "An integer-linear-programming-based routing algorithm for flip-chip design", IEEE TCAD 28(1), 2009 [A]; Lee, Lee, Ho, Chang, Chang, Lin, Shen, "Obstacle-avoiding free-assignment routing for flip-chip designs", DAC 2012 [A]:
  <https://scholars.lib.ntu.edu.tw/entities/publication/e5525489-cdd1-47b3-ae8f-9bdf1c9e2a87>
- Dai, Kong, Sato, "Routability of a rubber-band sketch", DAC 1991, pp. 45-48 [A, record only]: <https://dblp.dagstuhl.de/rec/conf/dac/DaiKS91.html>
- Dayan and Dai, "Layer assignment for rubber band routing", UCSC-CRL-93-04 [V abstract]:
  <https://tr.soe.ucsc.edu/sites/default/files/technical-reports/UCSC-CRL-93-04.pdf>
- Staepelaere, "Geometric transformations for a rubber-band sketch", MS thesis, UCSC-CRL-92-39 [A]:
  <https://tr.soe.ucsc.edu/sites/default/files/technical-reports/UCSC-CRL-92-39.pdf>
- Seong, Yang, Han, "Topology for substrate routing in semiconductor package design", arXiv 2105.07892 [V abstract]: <https://arxiv.org/pdf/2105.07892>
- gEDA/PCB toporouter and TopoR [A]: <https://en.wikipedia.org/wiki/PCB_(software)>, <https://en.wikipedia.org/wiki/TopoR>, <https://ssalewski.de/Router.html.de>
- Panel slide, Yih-Lang Li, ISPD 2022, "PCB routing" [V]: <https://ispd.cc/ispd2027/slides/2022/protected/14_6_Yih_Lang_Li.pdf>
- Hadlock, SIAM J. Comput. 4(3):221-225, 1975 [A]; Marek-Sadowska, IEEE TCAD 3(3):184-190, 1984, doi 10.1109/TCAD.1984.1270074 [A];
  Naclerio, Masuda, Nakajima, "The via minimization problem is NP-complete", Univ. of Maryland ISR TR 1987-42 [A]:
  <https://drum.lib.umd.edu/items/28992c46-bae6-47fa-8d53-602d4b5c75ca/full>
- Okamura-Seymour statement and extensions [A]: <https://arxiv.org/abs/2007.01280>
- Vendor material on dog-bone versus via-in-pad [A]: <https://resources.altium.com/p/which-bga-pad-and-fanout-strategy-right-your-pcb>,
  <https://jlcpcb.com/blog/effective-escape-routing-strategies>, <https://www.allpcb.com/allelectrohub/via-in-pad-design-for-bga-packages-a-step-by-step-guide>

Exact methods, route-variable encodings, ILP selection

- Nam, Aloul, Sakallah, Rutenbar, "A comparative study of two Boolean formulations of FPGA detailed routing constraints", ISPD 2001 [V]:
  <https://www.cecs.uci.edu/~papers/compendium94-03/papers/2001/ispd01/pdffiles/p222.pdf>
- Nam, Sakallah, Rutenbar, "Satisfiability-based layout revisited: detailed routing of complex FPGAs via search-based Boolean SAT", FPGA 1999 [V abstract]:
  <https://www.cecs.uci.edu/~papers/compendium94-03/papers/1999/fpga99/pdffiles/07_2.pdf>
- Jiang, Khatri, Sangiovanni-Vincentelli, Brayton, "A multi-layer area routing methodology using a Boolean satisfiability based router", UCB/ERL M99/16 [V]:
  <https://www2.eecs.berkeley.edu/Pubs/TechRpts/1999/ERL-99-16.pdf>
- Qi, Hu, Peng, You, Han, Zhu, "A high performance detailed router based on integer programming with adaptive route guides", ASP-DAC 2024 (slides) [A]:
  <https://www.aspdac.com/aspdac2024/archive/pdf/9D-3.pdf>
- Han, Kahng, Lee, "Evaluation of BEOL design rule impacts using an optimal ILP-based detailed router", DAC 2015, and Jia, Cai, Zhou, Yu,
  "A multicommodity flow-based detailed router with efficient acceleration techniques", IEEE TCAD 37(1), 2018 [A, known only from the
  Xidian slides' references].
- Lam, Le Bodic, Harabor, Stuckey, "Branch-and-cut-and-price for multi-agent pathfinding", IJCAI 2019 and the journal version [V abstract]:
  <https://ijcai.org/proceedings/2019/179>, <https://people.eng.unimelb.edu.au/pstuckey/papers/bcp-journal.pdf>

MAPF and LNS

- Li, Chen, Harabor, Stuckey, Koenig, "MAPF-LNS2: fast repairing for multi-agent path finding via large neighborhood search", AAAI 2022 [V]:
  <https://ojs.aaai.org/index.php/AAAI/article/view/21266>
- Tan, Luo, Li, Ma, "Reevaluation of large neighborhood search for MAPF: findings and opportunities", arXiv 2407.09451 [V abstract]: <https://arxiv.org/pdf/2407.09451>
- Okumura, Machida, Defago, Tamura, "Priority inheritance with backtracking for iterative multi-agent path finding", IJCAI 2019 / AIJ 2022 [V]: <https://arxiv.org/abs/1901.11282>
- Okumura, "LaCAM: search-based algorithm for quick multi-agent pathfinding", AAAI 2023 [A]: <https://arxiv.org/pdf/2211.13432>

VLSI and FPGA routing

- McMurchie and Ebeling, "PathFinder: a negotiation-based performance-driven router for FPGAs", FPGA 1995 [A]: <https://www.doi.org/10.1145/201310.201328>
- Nair, "A simple yet effective technique for global wiring", IEEE TCAD 6(2), 1987, doi 10.1109/TCAD.1987.1270260 [A]; Linsker, "An iterative-improvement penalty-function-driven wire routing system", IBM J. Res. Dev. 28(5):613-624, 1984 [A]
- Chang, Lee, Wang, "NTHU-Route 2.0: a fast and stable global router", ICCAD 2008 [A]: <https://websrv.cecs.uci.edu/~papers/iccad08/PDFs/Papers/05A.1.pdf>, <https://cs.nthu.edu.tw/~tcwang/nthuroute>
- Liu et al., "CUGR: detailed-routability-driven 3D global routing with probabilistic resource model", DAC 2020 [A]:
  <https://scholars.hkbu.edu.hk/en/publications/cugr-detailed-routability-driven-3d-global-routing-with-probabili/>
- Li, Chen, Jiang, Chen, Young, "Dr. CU 2.0: a scalable detailed routing framework with correct-by-construction design rule satisfaction", ICCAD 2019 [A, record only]: <https://dblp.uni-trier.de/pid/163/2323>
- Gester, Muller, Nieberg, Panten, Schulte, Vygen, "BonnRoute: algorithms and data structures for fast and good VLSI routing", ACM TODAES 18(2), 2013 [A]: <https://doi.org/10.1145/2442087.2442103>
- KiCad PNS (`pns_shove`, routing settings) [A]: <https://docs.kicad.org/doxygen/pns__shove_8cpp.html>; Freerouting [A]: <https://github.com/freerouting/freerouting/>
  and a third-party reading of its search: <https://tinycomputers.io/posts/the-mathematics-of-pcb-trace-routing.html>
