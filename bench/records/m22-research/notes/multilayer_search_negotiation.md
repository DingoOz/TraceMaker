# Multilayer search, layer assignment, via usage and negotiation for PCB completion

Scope note for the report writer. The tool budget was small (about 20 calls), and several primary PDFs could not be read:
the UCI paper mirror (ICCAD/ASP-DAC 2008 NTHU-Route) refused connections, and the TritonRoute-WXL PDF downloaded but
could not be turned into text (no PDF tools on this machine). So the cited findings below are mostly from abstracts,
project pages, slides and one full arXiv HTML paper (PCBWorld) plus one full arXiv HTML paper (Haas 2026 BGA bus).
Each item says how deep the reading was. Material from background knowledge of well-known papers that could not be
re-checked in this session is kept out of "Cited Findings". It appears in "Inferences" marked **[unverified
background]**, so the writer can drop it or check it.

## Q1. How do the best maze/A* routers handle layers so a connection blocked on one layer finds a two-via detour on another? Which heuristics or cost fields make this cheap?

### Takeaway
No source found in this session gives a published "layer-aware admissible A* heuristic" for 3D routing grids. The state
of the art handles layers in other ways. Global routers either route directly in 3D (FGR, CUGR multi-level 3D maze)
or route in 2D and then assign layers (FastRoute 4.0, NTHU-Route 2.0 + COLA, NCTU-GR). Detailed routers (TritonRoute,
Dr. CU) run 3D A* with wire and via costs plus violation penalties, and precompute **pin access points** before
searching. For TraceMaker's problem (a strict search floods its window because a pin is boxed in), the transferable
ideas are three. First, analyse pin access ahead of the search. Second, try cheap 3D *pattern* routes (L/Z shapes with
a layer per segment, chosen by dynamic programming) before falling back to a maze search. Third, use a cost-to-go
field that is itself computed in 3D (per layer, with vias), not a 2D distance plus a via term.

### Cited Findings
- Admissibility: an A* heuristic that never overestimates the true remaining cost gives an optimal search. For planar
  Manhattan routing the usual heuristic is Manhattan distance to the target. (Abstract/summary level.) —
  [UI-Route, SLIP 2014](https://tsung-wei-huang.github.io/papers/SLIP_2014.pdf)
- CUGR (DAC 2020, CUHK) is a "detailed-routability-driven 3D global router with a probabilistic resource model". It
  uses multi-level 3D maze routing. (Abstract/search snippet only.) —
  [CUGR, CUHK record](https://research.cuhk.edu.hk/en/publications/cugr-detailed-routability-driven-3d-global-routing-with-probabili-2/);
  [CUGR PDF](https://cwpui.com/doc/c10.pdf)
- FGR applies maze routing directly in 3D, with costs from Lagrange multipliers. (Search snippet only.) —
  [CUGR PDF, related-work summary](https://cwpui.com/doc/c10.pdf)
- 3D routing grids are usually modelled as stacked layers, each with a preferred direction. (Snippet.) —
  [Hexagonal Array Machine for Concurrent Multilayer Maze Routing (Mazumder)](https://web.eecs.umich.edu/~mazum/PAPERS-MAZUM/HAM.pdf)
- Dr. CU (ISPD 2019 detailed-routing contest winner): 3D path search on a sparse grid graph. Edge cost = base wire or
  via cost + out-of-guide penalty + short and spacing violation penalties. Min-area rules are handled inside the path
  search, not by a post-fix. Pins get access points first, then rip-up-and-reroute iterations run. Quality depends
  strongly on net order. It reports up to 65% (avg 39%) better quality than the ISPD 2018 winner, 80-93% less memory
  and 2.5-15x speed-up. Dr. CU 2.0 is about 2% better than the best ISPD'19 scores, and its code is public.
  (Search-summary level from the ASP-DAC 2019 talk, DATE 2021 and the Dr. CU 2.0 abstract. Methods sections not read.) —
  [ASP-DAC 2019 10C-1](https://www.aspdac.com/aspdac2019/archive/pdf/10C-1.pdf);
  [DATE 2021 paper on net ordering](https://past.date-conference.com/proceedings-archive/2021/pdf/1334.pdf);
  code at github.com/cuhk-eda/dr-cu
- DATE 2021: net order changes via count and DRC violations even when wirelength barely changes. (Snippet.) —
  [DATE 2021](https://past.date-conference.com/proceedings-archive/2021/pdf/1334.pdf)
- TritonRoute (Kahng, Wang, Xu, TCAD 2020, doi:10.1109/TCAD.2020.3003234) has pin access analysis, track assignment,
  initial detailed routing, and a "search and repair" loop with a built-in DRC engine. The loop runs up to 64
  iterations, configurable. Against the best academic detailed routers it reports wirelength up to 0.8% better (avg
  0.4%), via count up to 16.1% better (avg 9.3%) and DRCs up to 100% fewer (avg 92.0%). The successor, TritonRoute-WXL
  (TCAD 2021, doi:10.1109/TCAD.2021.3079268), says an integrated DRC engine is "highly desired due to the nature of
  per-net ripup-and-reroute". (Docs and abstract level; the A* cost terms were not readable.) —
  [OpenROAD drt docs](https://openroad.readthedocs.io/en/latest/main/src/drt/README.html);
  [TritonRoute-WXL PDF](https://vlsicad.ucsd.edu/Publications/Journals/j136.pdf)
- FastRoute 4.0 (Xu, Zhang, Chu, ASP-DAC 2009, pp. 576-581, "Global Router with Efficient Via Minimization"): the flow
  is via-aware Steiner tree, then pattern routing, then 3-bend routing and maze routing to remove congestion. When
  overflow stops falling, a carefully ordered net-by-net and edge-by-edge layer assignment finishes the job.
  (Project page and abstract level; no 4.0 numbers retrieved.) —
  [FastRoute project page](https://home.engineering.iastate.edu/cnchu/FastRoute.html);
  [ASP-DAC 2009 session 6B](https://www.aspdac.com/aspdac2009/archive/program/6B_abst.html);
  [OpenROAD FastRoute README](https://openroad.readthedocs.io/en/latest/main/src/grt/src/fastroute/README.html)

### Inferences
- **Admissible layer-aware lower bound (own reasoning, not from a source):**
  h(p, ℓ) = d_xy(p, T) · c_min + c_via · min over target layers |ℓ − ℓ_T| (or 0 if the target pin is through-hole and
  present on ℓ). This stays admissible as long as every step costs at least c_min and every via at least c_via. It is
  weak in exactly TraceMaker's failure case: it never "sees" that layer ℓ is walled off near the pin while ℓ' is open.
  So A* expands every node with f below the true cost on ℓ before it pays the via.
- **3D true-distance cost-to-go (own reasoning):** TraceMaker already builds cost-to-go fields by line sweeps. If those
  fields are built **jointly over all layers**, with a via edge between layers at every legal via site (a 3D
  Dijkstra or sweep from the target on a coarse lattice with obstacles inflated by clearance), the heuristic becomes
  the exact relaxed distance. A pin that is boxed in on the top layer but open via a nearby via site then gets a low
  h only on the path through that via. This is the "true-distance heuristic" / pattern-database idea from AI search.
  It stays admissible if the coarse lattice under-approximates obstacles (deflate, do not inflate, for admissibility;
  inflate if you accept a bounded-suboptimal search). A cheap version keeps 2D fields per layer and combines them
  with a min over "via-to-ℓ' at the nearest legal via site" (two-layer min-plus).
- **Pattern probes before maze (inference from FastRoute/CUGR flows):** The user's "two vias and a short run on another
  layer" is a 3D Z-pattern or U-pattern. Global routers try L, Z and 3-bend patterns before maze routing because they
  cost O(bbox) instead of O(window area). A PCB router could enumerate, for each failed connection, layer sequences
  (ℓ_src → ℓ' → ℓ_dst) with candidate via sites from a small set near each pin's escape, check each segment with the
  exact geometry check, and only flood when every probe fails.
- **Pin access analysis first (inference from TritonRoute and Dr. CU):** Both precompute legal access points (and via
  options) per pin before routing nets. For TraceMaker this means a cheap "escape probe" per pin per layer (can this
  pad reach open space on layer ℓ within k steps, or via-down within r?). It turns "flood the window, then discover
  the pin is boxed in" into an O(1) lookup that steers the search to start from the open escape (multi-source seeding
  from the pin's legal escape points on all layers).
- **Multi-source / multi-target seeding [unverified background]:** NTHU-Route 2.0's implementation techniques are
  recalled as including "adaptive multi-source multi-sink maze routing". Seeding the search from all points of the
  routed tree and terminating at any point of the target tree, on every layer the pad exists on, is standard.
  Verify against the ICCAD 2008 paper (link in Q3).
- **Classic search speed-ups [unverified background]:** Hadlock (1977, *Networks* 7) minimum-detour search: on a unit
  grid the detour number lets a BFS act like A* with integer buckets. Soukup (DAC 1978) "fast maze router": run
  depth-first toward the target and fall back to breadth when blocked. These matter for performance, not for layer
  awareness.

### Gaps
- No primary source read in this session describes the actual heuristic in TritonRoute, Dr. CU or CUGR's 3D maze. Code
  inspection (OpenROAD `src/drt`, github.com/cuhk-eda/dr-cu, github.com/cuhk-eda/cu-gr) would settle it.
- No paper found that measures how much a layer-aware heuristic shrinks A* expansions in multilayer PCB routing.
- Bidirectional A* for multilayer routing: no source found in this session.

## Q2. What does 2D global routing + layer assignment (NVM, COLA, via-count-aware LA) offer a PCB router whose problem is local congestion near packages?

### Takeaway
The VLSI "2D then layer assignment" pipeline minimises vias *given* a 2D solution that already has no overflow. It
does not by itself fix local congestion at pins. Its useful lessons are three: via count is optimised *after*
routability, layer assignment can be negotiated (NVM), and layer choice is cheap to do by dynamic programming over a
fixed 2D tree (COLA). The closer PCB analogue is **simultaneous escape routing and layer assignment** for dense
packages (Ozdal and Wong) and exact bus or escape planning with vias as decision variables (Haas 2026).

### Cited Findings
- COLA (Lee and Wang, TCAD 2008, congestion-constrained layer assignment): a single-net method for minimum via count
  of a 2D net. Later used by NTHU-Route-2.0-based GLADE. A later method extending COLA reduced via overflow against
  COLA at a small wirelength cost. (Slide level.) —
  [ISPD 2009 slides s8p2](https://ispd.cc/ispd2026/slides/2009/s8p2.pdf);
  [ISPD 2011 slides 3.2 Lee](https://ispd.cc/ispd2027/slides/2011/3.2_Lee.pdf)
- NVM (Liu and Li, ASP-DAC 2011, "Negotiation-Based Layer Assignment for Via Count and Via Overflow Minimization"):
  negotiation, PathFinder-style, applied to layer assignment. It reaches lower via counts than earlier methods, whose
  results depended on net order. A variant also targets via overflow. (Abstract level.) —
  [ASP-DAC 2011 session 6C abstracts](https://www.aspdac.com/aspdac2011/archive/program/6C_abst.html);
  [NCTU-GR page](https://people.cs.nycu.edu.tw/~whliu/NCTU-GR.htm)
- NCTU-GR (Dai, Liu, Li, ASP-DAC 2009; TVLSI 20(3) 2012): simulated-evolution-based rerouting plus congestion-relaxed
  layer assignment on 3D global routing. NCTU-GR 2.0 (Liu, Kao, Li, Chao, DAC 2010; TCAD 32(5) 2013): bounded-length
  maze routing, multithreaded collision-aware routing. Project tables: default mode leaves overflow on ISPD08 sb2 and
  sb4 (total overflow 1728 and 304). The "routability optimizer" makes both overflow-free and improves WL+via by
  2.7-12.2%. High-quality settings cost up to 6.4x runtime (sb2 2955 s → 19047 s). (Project page.) —
  [NCTU-GR page](https://people.cs.nycu.edu.tw/~whliu/NCTU-GR.htm);
  [NCTU-GR TVLSI record](https://ir.lib.nycu.edu.tw/handle/11536/15610)
- A DATE 2026 paper proposes congestion-aware layer assignment mapping 2D topology to 3D while minimising vias.
  (Snippet only.) — [DATE 2026 paper 1157](https://past.date-conference.com/proceedings-archive/2026/DATA/1157.pdf)
- Ozdal and Wong, "Simultaneous escape routing and layer assignment for dense PCBs" (ICCAD 2004; TCAD 25(8):1510-1522,
  2006): poses escape routing as longest path with forbidden pairs. An exact polynomial algorithm finds the maximal
  planar routing on one layer, and a randomised algorithm scales to large circuits. Escapes for several components are
  chosen at once so that crossings in the intermediate area are minimised. Ozdal, Wong and Honsinger (TCAD 27(1):84-95,
  2008) extend this to via minimisation for high-speed boards. (Abstract and reference level.) —
  [ICCAD 2004 PDF (aminer)](https://static.aminer.org/pdf/PDF/000/282/915/simultaneous_escape_routing_and_layer_assignment_for_dense_pcbs.pdf);
  [Illinois Experts TCAD record](https://experts.illinois.edu/en/publications/algorithms-for-simultaneous-escape-routing-and-layer-assignment-o/)
- Layer minimisation for staggered pin arrays (Ho, Shih, Chang, Cheng, ASP-DAC 2013). MORE-Router+ (ISQED 2024) uses
  ILP with bus-oriented layer assignment to minimise layers used. B-escape (ISPD 2010) does simultaneous escape by
  boundary routing. There is also a negotiated-congestion router for simultaneous escape routing. (Titles and abstracts.) —
  [Layer minimization staggered arrays](https://www.semanticscholar.org/paper/Layer-minimization-in-escape-routing-for-PCBs-Ho-Shih/143eb50a67313c78b462af6de1c82f6149d273cb);
  [MORE-Router+ PDF](https://numbda.cs.tsinghua.edu.cn/papers/isqed24_2.pdf);
  [B-escape](https://dl.acm.org/doi/10.1145/1735023.1735033);
  [Negotiated congestion router for simultaneous escape](https://www.researchgate.net/publication/224132327_A_negotiated_congestion_based_router_for_simultaneous_escape_routing)
- Haas (NYU), arXiv 2610.35601 (26 Oct 2026), "Whole-Route Planning of a Memory Bus Between Two BGAs: An Exact Model of
  Crossings and Layer Changes, Negotiated to DRC-Clean Copper". A CP-SAT model chooses, for all lanes at once, where
  forced crossings happen, their order (a "triple rule"), and where each lane changes layer: layer parity acts as Ising
  spins and vias as domain walls. An elastic LP (HiGHS) places lanes in mm, and violated constraints return as cuts.
  Plans snap to the router grid by A* and are audited against the production rules. Audit findings come back as
  PathFinder-style **history prices** and cuts. Results (full HTML read): every rung of two real ladders (Allwinner
  H3 → DDR3 up to 51 nets, Zynq-7020 → DDR3 up to 43 nets) is connected and DRC-clean, with fewer vias than the human
  designer (80 vs 88, 62 vs 100). The same toolkit's production chain without the bus step leaves nets open on 4 of
  10 rungs with 2.0-4.7x the vias. Freerouting 2.4.1 completes none of the rungs (1-5 nets open). On the real Zynq
  board: 45 bus nets complete with 70 vias (human 105). The production chain leaves 1 open with 162 vias; Freerouting
  leaves 2 open with 70. Synthetic: 2 layers 23/29 complete; on 3, 4 and 6 layers all 87 runs complete. Runtime is
  18-290 s per rung, 3.6-6.8x the production chain. —
  [arXiv 2610.35601](https://arxiv.org/html/2610.35601)

### Inferences
- For local congestion at packages, LA-after-2D works only if the 2D stage already models pin-area capacity
  correctly. VLSI routers do not face TraceMaker's boxed-in-pin situation in the same form, because pin access is
  solved separately (TritonRoute and Dr. CU pin access stages, Q1).
- The transferable ideas are: (a) **negotiated layer assignment** (NVM) as a cheap pass that moves segments of already
  found 2D paths between layers to free a blocked pin's layer; (b) **COLA-style DP over a fixed 2D path** to choose
  where to put the vias given a congestion map per layer. This is a direct way to turn "the 2D path exists; it just
  needs two vias" into a solution: find the 2D path ignoring the blocked layer's local obstacles, then run a DP over
  (position along path × layer) with via cost and per-layer blockage, then check exactly.
- The Haas 2026 result is the most PCB-relevant evidence in this session that **planning layer changes globally**
  (as decision variables, then negotiating with history prices) beats both Freerouting and a lattice-router chain on
  dense BGA-to-DRAM buses, in completion and via count. It is a single author's preprint on two real boards. The
  "production chain" it compares against may be the same author's toolkit; whether that is TraceMaker was not stated in
  what was read.

### Gaps
- COLA and NVM methods sections not read; via-count deltas (percent) not retrieved.
- No study found that applies VLSI LA to PCB routing and reports completion.

## Q3. Which rip-up-and-reroute / negotiation schemes are documented to finish the last few percent, and what do they cost? Any "last 5%" PCB studies?

### Takeaway
PathFinder's negotiated congestion (present-congestion × (base + history)) is the documented workhorse: FPGA routing
(VPR and all vendors) and VLSI global routing (NTHU-Route 2.0, Archer, FGR, NCTU-GR). It works because the first
iteration *allows sharing*, so every net gets a path and failure becomes overuse to negotiate away. Rip-up *ordering*
and congested-region identification (NTHU-Route 2.0) and simulated-evolution rerouting (NCTU-GR) are the documented
ways to finish the last overflow. Closing the last overflow costs runtime: NCTU-GR's high-quality mode takes up to
6.4x longer. No PCB-specific "last 5%" study was found.

### Cited Findings
- PathFinder (McMurchie and Ebeling, ACM FPGA 1995): a signal router (shortest path per net, with congestion and
  delay) and a global router that calls it repeatedly and adjusts resource costs. In the first iteration sharing is
  free. Node cost C_n = (b_n + h_n) · p_n, with h_n the congestion history from earlier iterations and p_n the present
  overuse. (Secondary sources: course pages and slides.) —
  [Lafayette PathFinder page](https://sites.lafayette.edu/cadapps/main-page/pathfinder-fpga-routing-algorithm/);
  [TU Darmstadt PathFinder slides](https://www.esa.informatik.tu-darmstadt.de/archive/twiki/pub/Lectures/AlgorithmenImChipEntwurfDe/router-pathfinder.pdf)
- Impact: all FPGA vendors' production routers use negotiated congestion or a generalisation of it. It is the
  cornerstone of VPR. It "changed FPGA routing from a major headache with wildly fluctuating results to a reasonably
  well controlled optimization problem." Later improved by Swartz, Betz and Rose (FPGA 1998), a fast routability-driven
  router. (UW news and lecture slides.) —
  [UW CSE news](https://news.cs.washington.edu/category/news/page/231/);
  [Darmstadt slides](https://www.esa.informatik.tu-darmstadt.de/archive/twiki/pub/Lectures/AlgorithmenImChipEntwurf07De/ace08.pdf)
- NTHU-Route 2.0 (Chang, Lee, Wang, ICCAD 2008): a history-based cost function, new ordering methods for
  congested-region identification and rip-up-and-reroute, and two implementation techniques. "The order of rip-up
  and reroute influences the solution quality." Edge cost = base + history × present congestion, so history dominates
  on congested edges. Results: all ISPD98; 7/8 ISPD07 with zero overflow; 12/16 ISPD08 with zero overflow; best on
  11/16 at the ISPD08 contest (1st place). Code needs a signed agreement. (Thesis abstract, ASP-DAC slides, project
  page. The ICCAD PDF host refused connections, so the exact formula and parameters were not read.) —
  [ICCAD 2008 PDF (unreachable this session)](https://websrv.cecs.uci.edu/~papers/iccad08/PDFs/Papers/05A.1.pdf);
  [NTHU thesis record](https://scholars.nthu.edu.tw/esploro/outputs/graduate/NTHU-Route-20-A-Fast-and-Stable/9957798169606774);
  [ASP-DAC 2008 slides 3A-2](https://www.aspdac.com/aspdac2008/Archive_Folder/3A_Slides/3A-2.pdf);
  [NTHU-Route page](https://cs.nthu.edu.tw/~tcwang/nthuroute)
- FastRoute (whole suite, VLSI Design 2012 journal): 12 of 16 ISPD07/08 benchmarks congestion-free, much faster than
  other routers. (Abstract.) — [FastRoute journal](https://lib.dr.iastate.edu/ece_pubs/103)
- NCTU-GR cost of closing: default mode leaves ISPD08 sb2/sb4 overflowed. High-quality settings raise sb2 runtime
  from 2955 s to 19047 s. A separate "routability optimizer" makes sb2/sb4 overflow-free. —
  [NCTU-GR page](https://people.cs.nycu.edu.tw/~whliu/NCTU-GR.htm)
- Detailed routing: Dr. CU and TritonRoute both use iterative rip-up and reroute with violation penalties in the edge
  cost. TritonRoute calls the loop "search and repair" and allows up to 64 iterations. —
  [OpenROAD drt docs](https://openroad.readthedocs.io/en/latest/main/src/drt/README.html);
  [ASP-DAC 2019 Dr. CU](https://www.aspdac.com/aspdac2019/archive/pdf/10C-1.pdf)
- Learning-based convergence of detailed-routing rip-up is an active topic: offline RL to speed DR convergence
  (arXiv 2512.03594), and history-aware offline RL with LSTM (arXiv 2609.08232). (Titles only.) —
  [arXiv 2512.03594](https://arxiv.org/pdf/2512.03594); [arXiv 2609.08232](https://arxiv.org/pdf/2609.08232)
- PCB: Haas 2026 negotiates DRC audit findings back as history prices, closing dense buses that Freerouting leaves 1-5
  nets open. — [arXiv 2610.35601](https://arxiv.org/html/2610.35601)

### Inferences
- **Directly relevant to "strict search floods, negotiation never reached":** PathFinder's key move is that iteration 1
  *permits* overuse, so the search always terminates with a path and congestion becomes data (history). TraceMaker's
  strict-first pass inverts this: a hard obstacle at a boxed-in pin causes a full-budget flood with no information
  gained. Options: (a) cap the strict search tightly (work units) and on failure go straight to a soft-obstacle search
  in which other nets' copper costs p·(b+h) instead of being a wall; (b) run the soft search first for connections
  whose pins fail the access probe (Q1). Locked items and keep-outs stay hard (rule 6).
- **Rip-up ordering and region focus (NTHU-Route 2.0, NCTU-GR simulated evolution):** rank connections by how much
  they occupy congested cells near failed pins, and rip those first, not by net order. Dr. CU and DATE 2021 show order
  sensitivity even in detailed routing.
- **Archer, BoxRouter 2.0, FGR [unverified background]:** Archer (Ozdal and Wong, ICCAD 2007) is a history-driven
  global router with Lagrangian-style cost updates. BoxRouter 2.0 (Cho, Pan et al., ICCAD 2007) is a negotiation-based
  A*/maze router with topology awareness. FGR (Roy and Markov, ICCAD 2007) treats history as discrete Lagrange
  multipliers in a 3D maze. All were ISPD07/08 contest routers. Exact numbers are not checked here.
- **TritonRoute search-and-repair schedule [unverified background]:** as recalled, each iteration reroutes nets in
  clips (tiles) around DRC markers, with clip offsets shifted between iterations so clip boundaries do not trap
  violations, and with the marker and fixed-shape costs raised on a schedule. This "windowed rip-up around violations
  with shifting windows" maps directly onto a lattice router's window searches. Verify in OpenROAD `src/drt/src/dr/`.
- **Freerouting [unverified background]:** ripup cost starts low and rises each pass; it routes with expansion rooms
  (gridless). PCBWorld (Q4) shows it at 0.78-0.80 clean pass on real 2-4 layer boards.

### Gaps
- No paper found that measures "the last 5%" on PCBs, or how many negotiation iterations PCB completion needs.
- Exact PathFinder constants (present-factor growth, history increment) not checked in the primary paper.

## Q4. PCB-specific multilayer routing papers, including topological, rubber-band and learning approaches, with completion results

### Takeaway
Classic PCB/MCM topological routing (SURF: Dai, Dayan, Staepelaere; Dai/Kong routability of rubber-band sketches;
Dayan/Dai layer assignment for rubber-band routing) represents wires as topology and defers geometry. This makes
rip-up cheap, and layer assignment is a separate step. Recent benchmarks (PCBWorld 2026) show rule-based routers still
lead on medium and larger real boards. Freerouting reaches 0.78-0.80 DRC-clean pass on 2-4 layer real boards. PPO
leads on small boards but falls to 0.45 clean on medium. LLM agents get 0.00 clean on medium boards.

### Cited Findings
- SURF: Dai, Dayan, Staepelaere, "Topological routing in SURF: generating a rubber-band sketch", DAC 1991, pp. 39-44.
  A multilayer topological router that uses hierarchical top-down partitioning to global-route all nets at once, with
  successive refinement. A rubber-band sketch has wires as elastic paths contracted to shortest length within their
  topology, so moving terminals and rerouting are cheap. The SURF system (Staepelaere, Jue, Dayan, Dai, IEEE Design &
  Test 1993) is a gridless, flexible routing system for MCMs. Related: "Routability of a rubber-band sketch" (Dai,
  Kong, Sato, 1991); "Layer assignment for rubber band routing" (Dayan and Dai, 1993); Dayan and Cruz 1997, a
  mathematical formalisation and a proved shortest-path algorithm. (Abstracts and citation records only.) —
  [ACM DL DAC 1991](https://dl.acm.org/doi/pdf/10.1145/127601.127622);
  [Semantic Scholar SURF figure](https://www.semanticscholar.org/paper/Topological-routing-in-SURF:-generating-a-sketch-Dai-Dayan/10b78bda6ccfccf178737bafac91ae80766a4ecf/figure/0);
  [Routability of a rubber-band sketch](https://www.semanticscholar.org/paper/Routability-of-a-rubber-band-sketch-Dai-Kong/103a2d8b33ce3de0710f3ab11de7f2f9c25947fd);
  [Layer assignment for rubber band routing](https://www.semanticscholar.org/paper/LAYER-ASSIGNMENT-FOR-RUBBER-BAND-ROUTING-Dayan-Dai/16960ca596cf4284769c1adbb488f2e578b3e282);
  [Rubber-band based topological router](https://www.semanticscholar.org/paper/RUBBER-BAND-BASED-TOPOLOGICAL-ROUTER-Dayan-Cruz/91ce7726d0b103db47ab5db433ed75b538e6e7f8)
- Topological routing for package substrates is a continuing thread: "Topological routing to maximize routability for
  package substrate" (2008), and "Topology for substrate routing in semiconductor package design" (arXiv 2105.07892).
  The gEDA "toporouter" follows SURF-style papers. (Titles and thread only.) —
  [ResearchGate substrate topological routing](https://www.researchgate.net/publication/4346821_Topological_routing_to_maximize_routability_for_package_substrate);
  [arXiv 2105.07892](https://arxiv.org/pdf/2105.07892);
  [gEDA toporouter thread](https://geda-user.delorie.narkive.com/LRF9gr2h/need-papers-of-toporouter)
- PCBWorld (arXiv 2607.05915, KDD 2026 eval workshop), full results table read. Datasets: D2 = 10k synthetic gridless
  2-layer boards; D3 = 679 real KiCad boards (D3-A 100 small, 2-4 layers; D3-B 287 medium, 2-4 layers; D3-C 292 large,
  2-8 layers, challenge split, **no results reported**). CP = DRC-clean full connectivity; Rout. = fraction of
  connections routed (@5 protocol; comparison subsets 99 D3-A and 10 D3-B boards):
  - Freerouting: D3-A CP 0.80 / Rout 0.91; D3-B CP 0.78 / Rout 1.00
  - KiCadRoutingTools (KRT): D3-A 0.74 / 0.94; D3-B 0.20 / 0.86
  - OrthoRoute: D3-A 0.02 / 0.53; D3-B 0.00 / 0.44
  - PPO: D3-A 0.86 / 0.95; D3-B 0.45 / 0.85. PPO without finish action: D3-A 0.94 / 0.99; D3-B 0.42 / 0.87. GRPO: D3-B 0.10 / 0.69
  - GPT-5.4 interactive: D3-A 0.65 / 0.91; D3-B 0.00 / 0.62. Engine-free GPT-5.4 on D3-A: CP 0.02.
  - PPO via count fell 4.06 → 2.82 → 2.27 as the via reward weight rose (Figure 9). There are no per-method via or
    layer statistics.
  - The abstract says learned methods still trail rule-based routers. PPO is trained only on synthetic boards and
    transfers zero-shot. TraceMaker is not among the evaluated routers. —
  [PCBWorld HTML](https://arxiv.org/html/2607.05915); [abstract](https://arxiv.org/abs/2607.05915)
- OmniRouting (arXiv 2608.04434): 1,681 industrial PCB designs with 2-8 layers, for LLM and multimodal evaluation, with
  human engineers as reference. It notes that earlier public benchmarks usually have 20 or fewer, mostly 2-layer,
  boards. (Abstract level; no tables read.) — [OmniRouting](https://arxiv.org/html/2608.04434v1)
- PCB-bench (Li et al., ICLR 2026) benchmarks LLMs on PCB placement and routing. Seen only as a citation. —
  [HWE-Bench citing it](https://arxiv.org/html/2603.18102)
- Bus planning and untangling on dense PCBs (Kong, Yan, Wong 2009; Yan and Wong 2007/2009; Yan et al. 2010
  differential-pair escape; Wen et al. 2022 layer assignment by maximum planar subset + LP) are cited as prior work by
  Haas 2026. (Citation list only.) — [arXiv 2610.35601](https://arxiv.org/html/2610.35601)

### Inferences
- Topological (rubber-band) routing addresses the user's symptom structurally. Deciding *which side of each obstacle
  and which layer* a wire takes is a discrete combinatorial choice, separate from geometry. A two-via detour is a
  small change to the sketch, while in a lattice A* it is a long expansion. A hybrid fits TraceMaker's rules: a
  topological or coarse 3D planner chooses layer sequences and via sites, and the lattice A* realises each segment
  under the exact geometric check. Haas 2026 follows this "plan topology + layers, snap with A*, audit, negotiate"
  pattern for buses.
- TopoR (Eremex) is a commercial any-angle topological PCB router. No academic primary source or published completion
  numbers were found in this session. Treat any claim about it as vendor material.
- Specctra / Cooper and Chang shape-based routing: no source retrieved in this session.

### Gaps
- No full text of SURF, Dai/Kong or Dayan/Dai was read; their completion numbers are unknown here.
- Specctra (shape-based) papers, DeepPCB, and Ozdal and Wong's book-length PCB routing treatment were not retrieved.
- PCBWorld reports nothing on D3-C (2-8 layers, up to 451 nets), so learned-vs-rule results on hard multilayer boards
  are missing.

## Q5. Benchmark numbers comparing completion with and without these techniques

### Takeaway
Only a few direct with/without comparisons turned up. Each is from the authors' own papers.

### Cited Findings
- Global layer-change planning (CP-SAT crossings and vias + negotiated history prices) vs the same toolkit without it:
  every rung complete and DRC-clean vs nets open on 4/10 rungs. Via count 2.0-4.7x lower. Freerouting 2.4.1 completes
  no rung. — [arXiv 2610.35601](https://arxiv.org/html/2610.35601)
- More layers make bus planning feasible: 2 layers 23/29 synthetic buses complete; 3, 4 and 6 layers 87/87. —
  [arXiv 2610.35601](https://arxiv.org/html/2610.35601)
- History-based cost and new ordering (NTHU-Route 2.0 vs its predecessor): 7/8 ISPD07 and 12/16 ISPD08 zero overflow;
  one ISPD07 case "previously considered difficult" fully cleared. —
  [NTHU thesis record](https://scholars.nthu.edu.tw/esploro/outputs/graduate/NTHU-Route-20-A-Fast-and-Stable/9957798169606774)
- Routability optimiser on vs off (NCTU-GR): ISPD08 sb2/sb4 go from overflow 1728/304 to 0. —
  [NCTU-GR page](https://people.cs.nycu.edu.tw/~whliu/NCTU-GR.htm)
- Via reward weight in PPO PCB routing: average vias 4.06 → 2.27, with wirelength also falling (484.8 → 462.4 mm under
  the wirelength-weight sweep). No completion change reported for the via sweep. —
  [PCBWorld](https://arxiv.org/html/2607.05915)
- Engine in the loop vs not (LLM agents, D3-A): CP 0.65 interactive vs 0.19 plan-and-execute vs 0.02 engine-free
  (GPT-5.4). — [PCBWorld](https://arxiv.org/html/2607.05915)

### Inferences
- The evidence leans toward "**complete first, minimise vias after**". VLSI flows run layer assignment and via
  minimisation after overflow is gone (FastRoute 4.0, NVM, COLA), and Haas 2026 finds that planning vias globally
  *reduces* both open nets and vias. A strict via budget in the search is not supported by anything found. A cheap
  via cost during completion, then a via-minimisation post-pass (move segments back to one layer where legal), fits
  the literature.
- For TraceMaker's portfolio, the "cheap via" variants are the closest analogue to the literature's
  route-then-minimise flows. They could become a pinned rescue stage for connections that fail the strict pass, with
  a via-removal pass afterwards.

### Gaps
- No controlled PCB study isolating (a) layer-aware heuristics, (b) pin access analysis or (c) soft-obstacle-first
  negotiation against completion rate.
- Via-cost-vs-completion trade-off curves for lattice PCB routers were not found.
