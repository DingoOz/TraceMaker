# Escape routing, pin access and via placement for dense PCB / package routing

Scope note: the notes say which papers were read in full and which only from abstracts or other papers' citations.
Read in full this session: Bayless, Hoos & Hu (ICCAD 2016) and Kahng, Wang & Xu "The Tao of PAO" (ISPD 2020).
Everything else comes from abstracts, bibliographic records, or descriptions in citing papers, and is marked
"(abstract)" or "(as cited by X)".

## 1. Escape-routing formulations, key papers, and what carries over to general PCBs (QFP/QFN/BGA)

### Takeaway
Single-layer escape routing with unordered or interchangeable destinations is a max-flow or min-cost-flow problem that
can be solved exactly in polynomial time. Ordered, differential-pair and multi-layer variants need MMCF/ILP, SAT,
LPFP, matching, or negotiated congestion. Multi-layer escape is not a pure flow problem because a via blocks the
neighbouring lattice nodes on every layer it passes through. That mutual exclusion between vias and traces is the
same thing that blocks fan-outs on TraceMaker's failing boards. The ideas that carry over to general PCBs are:
(a) treat "reach the component's escape boundary" as its own sub-problem solved for all pins of a component at
once; (b) treat via sites as choices that exclude their neighbours; (c) score an escape by how routable it leaves the
area outside the boundary (Ozdal's outside-routability conditions); (d) negotiated congestion, which is already
known to match a commercial router on escape problems.

### Cited Findings

**Survey and taxonomy**
- Bayless et al. sort the escape literature into four groups: (1) analytical or optimal single-layer solutions
  (Palios 1997; Yu & Dai ICCAD 1995; Ma, Kong, Wong & Young ASP-DAC 2011 "rectangle escape problem"); (2) design
  patterns and heuristics for multi-layer via placement (Wang, Shi & Cheng ICCAD 2006 route triangular sub-sections
  of each layer first; Shi & Cheng DAC 2006 column-by-column for hexagonal arrays; Horiuchi et al. 2000 on placing
  BGA pads and vias on the top layer); (3) network flow (Yu & Dai 1995; Wang et al. ASP-DAC 1999; Chan et al.
  Algorithmica 2003; Fang et al. ICCAD 2005 / TCAD 2007; Yan & Wong DAC 2009); (4) constraint-based ILP/SAT (Fang
  et al. TCAD 2009; Ho, Lee & Chang TCAD 2011; Ho et al. ASP-DAC 2013; Lei & Mak TCAD 2016; Luo & Wong ASP-DAC 2008)
  — [Bayless, Hoos & Hu, ICCAD 2016 (full text)](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- General disjoint paths between specific pairs is NP-hard (Middendorf & Pfeiffer 1993; Vygen 1995). When all
  destinations are the same or interchangeable it becomes max-flow or min-cost max-flow and is polynomial. The usual
  flow graph gives every node and edge capacity 1 (node capacity enforced by splitting the node). Some encodings use
  capacity 2-3 per node to scale — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Yan & Wong, "Recent research development in PCB layout," ICCAD 2010, is the standard survey of flow-based escape
  routing — [Bayless et al. ref 39](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf); [ACM DL record](https://dl.acm.org/doi/10.5555/2133429.2133514)

**Unordered (single-component) escape: network flow**
- Yan & Wong, "A correct network flow model for escape routing," DAC 2009. Earlier flow models could produce
  infeasible routings (e.g. with diagonal routing). Yan & Wong give a corrected flow model for grid pin arrays. Ho et
  al. (TCAD 2013) later gave one for staggered pin arrays (abstract / as cited) — [Semantic Scholar](https://www.semanticscholar.org/paper/A-correct-network-flow-model-for-escape-routing-Yan-Wong/32f72c8568add2df77cf4d3a28895d5ab098a8be); [ResearchGate (staggered-pin citation)](https://www.researchgate.net/publication/221627866_Escape_Routing_for_Staggered-Pin-Array_PCBs)
- Ho, Lee & Chang, "Escape routing for staggered-pin-array PCBs," TCAD 2011, and Ho, Shih, Chang & Cheng, "Layer
  minimization in escape routing for staggered-pin-array PCBs," ASP-DAC 2013, extend ILP/flow escape to staggered
  (hexagonal) arrays and to several layers — [Bayless et al. refs 14-15](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)

**Ordered escape routing (OER)**
- Luo & Wong, "Ordered escape routing based on Boolean satisfiability," ASP-DAC 2008. A SAT encoding of
  single-layer ordered escape. Bayless et al. say it supports no vias or multiple layers and reports scaling only to
  about 256 pins — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Kong, Yan, Wong & Ozdal, "Optimal bus sequencing for escape routing in dense PCBs," ICCAD 2007; Yan, Kong & Wong,
  "Optimal layer assignment for escape routing of buses," ICCAD 2009; Kong, Yan & Wong, "Optimal simultaneous pin
  assignment and escape routing for dense PCBs," ASP-DAC 2010. For buses (contiguous signals that must keep their
  order between two pin arrays) an optimal polynomial-time multi-layer algorithm is known —
  [Bayless et al. refs 19, 20, 37](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Jiao & Dong (as described by a citing paper) state three OER constraints (non-crossing, ordering, capacity) and
  turn the classic flow model into a min-cost multi-commodity flow (MMCF) for grid pin arrays (IEEE 2016) —
  [IEEE Xplore 7428042](https://ieeexplore.ieee.org/document/7428042); [ScienceDirect 2024, describing Jiao et al.](https://www.sciencedirect.com/science/article/abs/pii/S0167926024001342)
- "Ordered Escape Routing with Consideration of Differential Pair and Blockage," ACM TODAES 2018, extends the MMCF
  to differential pairs and thermal blockages and reports 100% routability on all its test cases (abstract) —
  [ACM DL 10.1145/3185783](https://dl.acm.org/doi/10.1145/3185783)
- "Two stage Ordered Escape Routing combined with LP and heuristic algorithm for large scaled PCB," Integration
  (Elsevier) 2024: an LP stage followed by a heuristic stage, aimed at scaling OER (abstract only; no numbers seen) —
  [ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0167926024001342)

**Simultaneous escape routing (SER): several components at once, routing between them**
- Ozdal & Wong, "Algorithms for simultaneous escape routing and layer assignment of dense PCBs," TCAD 25(8) 2006
  (ICCAD 2004). Escapes several components together while minimising crossings in the area between them, modelled as
  longest path with forbidden pairs (LPFP). There is an exact polynomial algorithm for the maximal planar subset on
  one layer and a randomized algorithm for large cases. Each layer takes the maximal planar subset of nets; the rest
  go to later layers. Tested on industrial circuits (abstract) — [ACM DL](https://dl.acm.org/doi/10.1109/TCAD.2005.857376); [Illinois Experts](https://experts.illinois.edu/en/publications/algorithms-for-simultaneous-escape-routing-and-layer-assignment-o/); [AMiner PDF](https://static.aminer.org/pdf/PDF/000/282/915/simultaneous_escape_routing_and_layer_assignment_for_dense_pcbs.pdf)
- Luo, Yan, Ma & Wong, "B-escape: a simultaneous escape routing algorithm based on boundary routing," ISPD 2010
  (abstract / record only) — [ACM DL](https://dl.acm.org/doi/10.1145/1735023.1735033); [ResearchGate](https://www.researchgate.net/publication/220915645_B-escape_A_simultaneous_escape_routing_algorithm_based_on_boundary_routing)
- Ma, Yan & Wong, "A negotiated congestion based router for simultaneous escape routing" (NCER), ISQED 2010,
  pp. 606-610. Applies PathFinder-style negotiated congestion to SER on a routing graph. On 14 industrial test cases,
  NCER and Cadence Allegro PCB router each completely route 7 (abstract) — [ResearchGate](https://www.researchgate.net/publication/224132327_A_negotiated_congestion_based_router_for_simultaneous_escape_routing); [ISQED 2010 archive](https://isqed.org/English/Archives/2010/Technical_Sessions/123.html)
- Later SER: a network-flow SER (2016, Malaysian J. Comp. Sci.), an ILP SER (2021), and "A dual model node based
  optimization algorithm for simultaneous escape routing in PCBs" (PeerJ CS 2021). Only the abstracts were reached;
  the PeerJ page returned HTTP 403 — [MJCS PDF](https://ejournal.um.edu.my/index.php/MJCS/article/download/7006/4659/15036); [PMC](https://pmc.ncbi.nlm.nih.gov/articles/PMC8056246/)

**Multi-layer BGA escape with via placement: SAT**
- Bayless, Hoos & Hu, "Scalable, high-quality, SAT-based multi-layer escape routing," ICCAD 2016 (full text read).
  Method: a "symbolic" flow graph per layer on a 45/90-degree lattice in which every position is routable. Candidate
  vias sit at regular intervals as Boolean variables. A constraint via -> NOT(a_i) disables the ring of neighbouring
  nodes the via's diameter would block, on each layer the via passes through. The blocked pattern is set by via width
  (about 1.5-2x trace width in the figures). Through-hole, blind, buried and micro-vias are different
  layer-span constraints on the same variables. A single constraint maxflow(s,t) >= |signals| is solved by MonoSAT's
  max-flow theory. A per-layer min-cost max-flow pass then shortens the traces, because SAT gives feasibility only and
  the traces "make completely unnecessary detours" — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Benchmarks: 23 commercial BGAs from TI, Lattice, Altera and Xilinx, from 28x28 (382 signals) to 54x54 (1,755
  signals plus 1,137 power/ground), at 0.8 mm and 1 mm pitch. Power and ground go to bottom planes on through-vias.
  Baseline: a greedy layer-by-layer max-flow router (route as many as possible on the top layer, then add a layer with
  vias for the rest) — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Results. Blind vias (Table 1): MonoSAT never needs more layers than the baseline and saves a layer on 13 of 23
  parts (e.g. TI AM5K2E04, 33x33: 3 layers vs 4, and proves 2 infeasible; Xilinx XCVU440 54x54: 3 vs 4). Through
  vias (Table 2): savings of up to 2 layers (TI TMS320C 29x29: 3 vs 5; TI AM52E02: 3 vs 5). Average trace length is
  about 5-10% longer. Runtime ranges from about 1.5 min to about 2.2 h on the largest part, against about 25 min for
  the baseline — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- The authors explain the gain: MonoSAT "left channels open at lower layers by routing several adjacent pads at the
  top layer (which does not require vias), instead of routing them at lower layers using through-vias. These channels
  then allowed large numbers of traces to be routed through the spaces left open at lower layers, which would
  otherwise have been mostly blocked by through vias." — [Bayless et al. 2016, §5](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Differential pairs: both members are forced onto the same layer (equal via variables). The single-layer diff-pair
  method of Yan, Wu, Ma & Wong (ICCAD 2010) then runs on each layer. When it fails, a lazy constraint is added ("this
  pair must be on a different layer from at least one signal between it and the package edge") and the problem is
  re-solved. Example: TI AM5728 with more than 20 pairs over 3 layers, 2 iterations, 204 s — [Bayless et al. 2016, §6](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Lei & Mak, "Optimizing pin assignment and escape routing for blind-via-based PCBs," TCAD 2016: an ILP for
  multi-layer escape with differential pairs that lets the solver choose package pin positions (co-design), so
  Bayless et al. say it is not directly comparable — [Bayless et al. ref 22](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)

**Differential-pair escape**
- Yan, Wu, Ma & Wong, "On the escape routing of differential pairs," ICCAD 2010; Li, Chen, Cai & Chen, "Escape
  routing of differential pairs considering length matching," ASP-DAC 2012 —
  [Bayless et al. refs 23, 40](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- "Simultaneous Constrained Pin Assignment and Escape Routing Considering Differential Pairs for FPGA-PCB Co-Design,"
  TCAD 2013 (record only) — [ACM DL](https://dl.acm.org/doi/abs/10.1109/TCAD.2013.2276536)

**Escape with real PCB rules (variable widths, necking, pad entry): matching**
- Liu, Lin, Chen, He, Chen & Chang, "A Matching Based Escape Routing Algorithm with Variable Design Rules and
  Constraints," DAC 2023. Pins are projected to the boundary with an obstacle-avoiding method into a channel
  projection graph, then a bi-projection graph, then a matching-based hierarchical sequencing step that respects
  manual constraints. It handles variable line width and spacing, wire "neck" mode, and differential-pair pad entry.
  Reports 100% routability with no DRC violations on all industrial PCB instances, while two state-of-the-art routers
  failed to complete them (abstract). A TCAD 2025 journal version adds length-aware detailed routing for diff pairs —
  [ACM DL](https://dl.acm.org/doi/abs/10.1109/DAC56929.2023.10247781?download=true); [NTU Scholars record](https://scholars.lib.ntu.edu.tw/handle/123456789/636607)

**Full-board flows built around escape**
- Lin, Wang, Kuo, Chen & Li, "A Complete PCB Routing Methodology with Concurrent Hierarchical Routing," DAC 2021.
  A concurrent hierarchical SER stage puts signals on a minimal number of layers and fixes escape order and pin
  locations per layer. Pin locations are then adjusted using the length-matching slack SER leaves, followed by
  layer-by-layer gridless area routing (described by a secondary source; no results found) —
  [NYCU Scholar](https://scholar.nycu.edu.tw/en/publications/a-complete-pcb-routing-methodology-with-concurrent-hierarchical-r/); [Iowa State thesis](https://dr.lib.iastate.edu/server/api/core/bitstreams/baa06fe6-541d-4f4a-888d-94f3083cd518/content)
- Ozdal, Wong & Honsinger, "Optimal routing algorithms for pin clusters in high-density multichip modules," ICCAD
  2005; TODAES 13(4) 2008. Studies how the positions of escape terminals on a convex boundary affect routability
  outside it. Gives necessary and sufficient conditions for routability outside a rectilinear convex boundary, an
  optimal algorithm for the largest set of nets routable outside the boundary given an escape, and an optimal
  algorithm for the best escape that meets all outside-routability constraints. Reports about 17% fewer layers on
  average (abstract) — [HKBU scholars record](https://scholars.hkbu.edu.hk/en/publications/optimal-routing-algorithms-for-rectilinear-pin-clusters-in-high-d/); [Crossref](https://api.crossref.org/works/10.1145%2F1391962.1391976)

**Related: other negotiated-congestion escape**
- McDaniel, Grissom & Brisk, "Multi-terminal PCB escape routing for digital microfluidic biochips using negotiated
  congestion," VLSI-SoC 2014. Extends negotiated congestion to multi-terminal escape and finds it more effective than
  maze routing with rip-up and re-route (abstract). The TCAD 2017 journal version, "PCB Escape Routing and Layer
  Minimization for DMFBs," relaxes pin assignment to cut layer count and finds that pathological pin assignments can
  sharply increase the number of PCB layers (abstract) — [VLSI-SoC PDF](https://webarchive.ucr.edu/public/archived-sites/microfluidics.cs/microfluidics.cs.ucr.edu/media/336/vlsisoc14-pcb_escape_routing4429.pdf); [TCAD 2017 PDF](https://webarchive.ucr.edu/public/archived-sites/microfluidics.cs/microfluidics.cs.ucr.edu/media/371/tcad17-pcb_escape_routing4429.pdf); [eScholarship](https://escholarship.org/uc/item/9862j8q0)

### Inferences
- Mapping, flow escape (Yan & Wong 2009 and later). For each dense footprint (BGA, QFN, QFP, fine-pitch connector),
  define an escape boundary (courtyard plus a margin). On the router's own lattice, clipped to that window, solve a
  unit-capacity max-flow from all the footprint's signal pads to the boundary. This tells TraceMaker cheaply, and
  exactly for that window, whether the fan-out is feasible on each layer before general routing. It also says which
  pins must change layer. The result can be kept as soft reservations: a per-net low-cost corridor, and a cost raise
  for other nets inside the window.
- Mapping, Bayless-style via sites. Their encoding is exactly "a via occupies a node and blocks a ring of neighbours
  on every layer it spans". In a negotiated lattice router the same idea fits as present-congestion and history cost
  charged on the via's blocking footprint on all spanned layers, not only on its centre node. Inside a footprint's
  escape window that cost could be raised, so a via that kills two escape channels pays for both.
- Bayless's explanation (top-layer routing of adjacent pads keeps lower-layer channels free of through vias) supports
  a cost rule: inside an escape window, a through-via should cost in proportion to the lower-layer channel capacity
  it consumes. The fixed via cost a lattice A* usually charges does not do this.
- Mapping, Ozdal 2005/2008 outside-routability. Score an escape by the routability it leaves outside the boundary
  (planarity, crossing count, and order of escape terminals against the far-end destinations), not only by whether
  every pin reaches the boundary. That is the link between "package fan-out" opens (335) and "channel away from pins"
  opens (259).
- Mapping, NCER (Ma et al. 2010). NCER is the strongest sign that TraceMaker's PathFinder machinery is a known-good
  base for escape. The missing piece is a dedicated escape phase with its own graph and its own history costs before
  the global negotiated pass, not a new algorithm.
- What carries over to QFP/QFN: these are perimeter-pin parts, so there is no "inner ring" and the escape is
  essentially one row. Ordered escape and bus sequencing (Kong et al.) still matter: the order in which pins leave
  the QFN edge against their far-end order decides crossings, hence vias. Simultaneous escape (Ozdal & Wong 2006;
  B-escape; DAC 2021) is the right model when a QFN escapes toward a BGA or connector.

### Gaps
- Full text and exact numbers were not obtained for Yan & Wong DAC 2009, Ozdal & Wong TCAD 2006, NCER (beyond 7/14),
  B-escape, the MMCF OER papers, the DAC 2021 methodology, or the DAC 2023 matching paper. Their claims are
  abstract-level.
- No paper found that targets QFN/QFP fan-out on mixed-technology boards specifically. The literature is almost all
  grid or staggered BGA arrays, buses, or flip-chip/RDL.
- Wang, Shi & Cheng ICCAD 2006 (triangular sub-section heuristic, layer minimization) and Kubo & Takahashi ISPD 2005
  (2-layer BGA global routing) were found only in reference lists.

## 2. Pin-access ideas from VLSI detailed routing that transfer to a PCB lattice router

### Takeaway
VLSI detailed routers handle pin access with an "oracle" that runs before routing. For each pin it enumerates a small
set (k about 3) of design-rule-clean access points with allowed directions and a preferred via. It then picks, by
dynamic programming over neighbouring pins and neighbouring instances, a mutually compatible access pattern so that
one pin's access never makes a neighbour's impossible. In the PAO paper this took failed pins on ISPD-2018 from
thousands to 0, and final DRCs on one hard case from 755 (best prior router) to 2. The transferable ideas are
precomputed, DRC-checked access points per pad, joint compatibility between neighbouring pads, and treating access
points as reserved endpoints the router must hit.

### Cited Findings
- Kahng, Wang & Xu, "The Tao of PAO: Anatomy of a Pin Access Oracle for Detailed Routing," ISPD 2020 (full text
  read). An access point is an (x, y) on a pin shape where the router ends, storing the valid access directions
  (planar E/W/N/S or "up" via) and, for via access, the valid vias with one marked primary. An access pattern is one
  access point per pin of a unique instance whose primary vias are mutually DRC-clean — [Tao of PAO PDF](https://vlsicad.ucsd.edu/Publications/Conferences/377/c377.pdf)
- Access point generation: four coordinate types, cheapest first: on-track (0), half-track (1), shape-centre (2),
  enclosure-boundary (3). Candidates are tried in cost order. A candidate is valid only if a via can be dropped
  DRC-free (checked with the real DRC engine). Generation stops once at least k = 3 valid points exist per pin. "Too
  small a number ... overly restrict[s] the solution space ... too large a number ... provide[s] excessive options ...
  again resulting in degraded solution quality" — [Tao of PAO](https://vlsicad.ucsd.edu/Publications/Conferences/377/c377.pdf)
- Access pattern generation: pins are ordered by x_avg + 0.3*y_avg. A layered graph has one group of nodes per pin
  and complete bipartite edges between consecutive pins. Edge cost is apCost(prev) + apCost(curr), a large drcCost if
  the two access points conflict (also checked one pin further back, "history-aware"), and a penalty for re-using
  boundary-pin access points already used in earlier patterns. This yields up to 3 diverse patterns per instance with
  different boundary-pin choices — [Tao of PAO](https://vlsicad.ucsd.edu/Publications/Conferences/377/c377.pdf)
- Cluster-based selection: abutting instances in a row form a cluster, and a second DP picks one pattern per instance
  so that neighbouring instances' boundary access points are compatible — [Tao of PAO](https://vlsicad.ucsd.edu/Publications/Conferences/377/c377.pdf)
- Results on ISPD-2018 (10 designs, up to 290K cells and 790K pins, 45/32 nm): original TritonRoute 0.0.6.0 generated
  up to 469 dirty access points per design; PAAF generated 0 dirty, with more points and about half the runtime.
  Failed pins (no DRC-clean access point after intra- and inter-cell compatibility) went from 31-7,816 per design to
  0 on all 10 with BCA (80 failed on test5 without BCA). Runtime was under about 2 min single-threaded. On
  ispd18_test5 the final routed DRC count was 2, against 755 for Dr. CU 2.0, the best published result — [Tao of PAO, Tables II-III](https://vlsicad.ucsd.edu/Publications/Conferences/377/c377.pdf)
- PAO's account of prior work: Ozdal, "Detailed-routing algorithms for dense pin clusters in integrated circuits,"
  TCAD 28(3) 2009, used multicommodity flow with Lagrangian relaxation for escape routing of dense pin clusters, with
  "limited scalability due to its per-net analysis nature". Xu et al. (PARR, TODAES 2016; "Concurrent pin access
  optimization for unidirectional routing," DAC 2017; TCAD 2015) did cell- and instance-level pin access planning but
  assume 1D gridded design and use large hit-point lookup tables. Ding, Chu & Mak (ISPD 2017) and Seo et al. (DAC
  2017) do pin-access-driven detailed placement or cell redesign — [Tao of PAO §I and refs](https://vlsicad.ucsd.edu/Publications/Conferences/377/c377.pdf)
- TritonRoute lists pin access analysis as a core building block alongside track assignment, initial detailed
  routing, search-and-repair, and DRC. The ISPD-2018 TritonRoute paper generated a minimum of K = 3 access points per
  pin — [OpenROAD drt docs](https://openroad.readthedocs.io/en/latest/main/src/drt/README.html); [TritonRoute journal paper](https://vlsicad.ucsd.edu/Publications/Journals/j133.pdf)
- Dr. CU (Chen et al., "Detailed Routing by Sparse Grid Graph and Minimum-Area-Captured Path Search," ASP-DAC 2019 /
  TCAD) improved on the ISPD-2018 contest winner by 39% on average (up to 65%) by contest metric and won ISPD 2019.
  Dr. CU 2.0 adds handling of hard-to-access pins. FastPass (a later fast pin-access analysis framework) was
  integrated into Dr. CU (abstracts and secondary) — [Cadence ISPD 2019 blog](https://community.cadence.com/cadence_blogs_8/b/can/posts/international-symposium-on-physical-design-2019); [FastPass record](https://scholars.hkbu.edu.hk/en/publications/fastpass-a-fast-pin-access-analysis-framework-for-detailed-routab/); [ML P&R survey](https://arxiv.org/pdf/2202.13564)
- The ISPD-2018 contest organisers named pin-access location selection and via selection among the contest's main
  challenges — [ISPD 2018 contest slides](https://ispd.cc/ispd2026/slides/2018/s7_3.pdf); [ISPD 2018 contest page](https://www.ispd.cc/contests/18/index.html)
- BonnRoute (IBM/Bonn) uses a shape-based data structure for pin access with exact shortest-path algorithms, and a
  two-level track structure for long connections. Nieberg (DAC 2011) studied gridless pin access in detailed routing.
  The Bonn slides admit their simple heuristic "does not find all offgrid access paths" (abstract and slides) —
  [IBM Research](https://research.ibm.com/publications/detailed-routing-algorithms-for-advanced-technology-nodes); [PARR paper (citing Nieberg)](https://www.cerc.utexas.edu/utda/publications/C178.pdf); [Schulte slides](https://ctw08.di.unimi.it/slides/B1/B1-2-Schulte.pdf)

### Inferences
- Mapping, access points on PCB pads. For every SMD pad, generate a few (about 3) access points: the pad centre for
  via-in-pad when allowed, plus short-stub ends on each free side (the dog-bone via site or the trace exit
  direction). Validate each against the real clearance rules with the exact geometric checker, as PAO validates with
  the real DRC engine. A lattice A* then targets "any valid access point of this pad" instead of "any lattice node
  inside the pad".
- Mapping, joint compatibility (PAO step 2-3) for QFN/QFP rows and BGA rings. Order pads along the footprint edge (or
  ring by ring) and run the same chain DP so that neighbouring pads' chosen exits and via sites are mutually
  clearance-clean. Generate 2-3 diverse patterns, as PAO uses boundary penalties to get diversity, and let
  negotiation choose among them. Then reserve the chosen pattern: other nets pay a high, history-raised cost to enter
  another pin's selected access point or its via-site blocking ring.
- That reservation is a direct answer to failure (a), "other nets' vias in a footprint's fan-out region". PAO's core
  result is that un-coordinated access (old TritonRoute) leaves thousands of pins with no clean access, while
  coordinated access leaves none.
- PAO's warning about too many access points applies to a cost-based A*: offering every lattice node on a pad as an
  endpoint lets the search pick exits that block neighbours. Limiting endpoints to a few validated points is a
  regulariser.
- Ozdal TCAD 2009's per-net multicommodity flow plus Lagrangian relaxation for dense IC pin clusters is close to what
  a per-footprint escape solve would be. PAO says it did not scale to whole chips; per-footprint it should be small.

### Gaps
- No paper was found that applies VLSI pin-access oracles to PCB routing. The transfer is inference.
- "PAAG" and "pin access interference" as named techniques could not be confirmed in this search. Dr. CU's own
  pin-access method, FastPass's numbers, and via-pillar-aware work were not read in full.
- The TritonRoute-WXL or OpenROAD drt follow-ups that use access-point graphs during search were not checked.

## 3. Via placement, via sites, via relocation/legalisation, and keep-out costs

### Takeaway
The strongest academic treatment of "a via blocking escapes" is the Bayless et al. via-site model: candidate vias as
Boolean variables that exclude their neighbouring nodes on every spanned layer, chosen together with all traces.
Explicit post-route via relocation or shoving for PCBs is close to absent from the academic literature found. The
nearest are post-route segment-shifting ILPs (Tseng et al.) and via-minimisation SER (Ozdal, Wong & Honsinger:
about 39% fewer vias). Practical design guidance (Lattice, Altium, embedded.com) codifies dog-bone versus via-in-pad
by pitch and pushing perimeter fan-out vias outward. These are heuristics that can become cost fields.

### Cited Findings
- Vias are typically 1.5x, 2x or 3x trace width, so "placing a via has the effect of reducing the space available for
  routing traces on the layers that the via passes through, sometimes substantially". Unless via positions are fixed
  in advance, multi-layer escape "cannot be directly modelled as a network flow problem", because via flow must
  occlude flow on adjacent edges on the layers above and below. With fixed via positions the problem splits into
  independent single-layer flows — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Via-site model: potential vias at regular intervals; via -> NOT(neighbour_i) for every blocked node on every layer
  spanned; through/blind/buried/micro-via spans as constraint templates; any via width by changing the blocked
  pattern. Bayless et al. report that all 23 BGAs could be escaped on 2-3 signal layers even with the restrictive
  through-hole and blind models — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Ozdal, Wong & Honsinger, "Simultaneous escape-routing algorithms for via minimization of high-speed boards," TCAD
  27(1) 2008, pp. 84-94 (ICCAD 2005 framework paper: "An escape routing framework for dense boards with high-speed
  design constraints"). Routes nets in several dense components together to minimise crossings, hence vias, in the
  area between them, under high-speed constraints. Reduces via requirements by 39% on average on industrial test
  cases against Ozdal & Wong 2004 (abstract) — [HKBU scholars record](https://scholars.hkbu.edu.hk/en/publications/simultaneous-escape-routing-algorithms-for-via-minimization-of-hi/); [ICCAD 2005 framework record](https://scholars.hkbu.edu.hk/en/publications/an-escape-routing-framework-for-dense-boards-with-high-speed-desi/)
- Tseng et al., post-route refinement for high-frequency PCBs: iterative segment swapping, detouring and shifting to
  free space, run until no improvement or an iteration cap. A companion ILP does prioritised shifting with
  progressive fixing. These are the nearest academic analogues to post-route object relocation (they move meander
  segments, not vias) — [arXiv 1705.04982](https://arxiv.org/pdf/1705.04982); [arXiv 1705.04984](https://arxiv.org/abs/1705.04984)
- "A Unified Printed Circuit Board Routing Algorithm With Complicated Constraints and Differential Pairs" (ASP-DAC
  2021) targets via minimisation for high-speed boards among other constraints (fragments only) — [ACM DL](https://dl.acm.org/doi/pdf/10.1145/3394885.3431568)
- Practitioner guidance (not peer-reviewed): dog-bone fan-out for ball pitch of 0.5 mm and above, via-in-pad below
  0.5 mm (embedded.com). Laser blind/buried via stacks allow dog-bone down to 0.5 mm (Altium). For BGAs under 0.8 mm
  pitch, push the first two rows of perimeter fan-out vias outward so the next rows can route on the same layer, and
  use quadrant dog-bone patterns (via direction rotated per quadrant) to raise density (Lattice) —
  [embedded.com](https://www.embedded.com/making-efficient-use-of-bga-signal-routing-in-pcb-designs/); [Altium](https://resources.altium.com/p/design-rules-to-fanout-a-large-bga); [Lattice whitepaper](https://www.latticesemi.com/-/media/LatticeSemi/Documents/WhitePapers/HM/LowCostBoardLayoutTechniquesforDesigningwithPLDsinBGAPackages.ashx?document_id=38558)
- Patent literature on ASIC pinout optimisation for HDI notes that PTH vias cannot sit directly under a BGA pad, so
  each pad routed internally needs a dog-bone via. The result is a full matrix of vias through most layers —
  [US 7757196](https://image-ppubs.uspto.gov/dirsearch-public/print/downloadPdf/7757196)
- Practitioner routing note: moving a via pair left or right to find alternative routing solutions, a manual
  via-shifting move in commercial routers — [Speeding Edge, "PCB Routers and Routing Methods"](https://www.speedingedge.com/PDF-Files/pcbrouters.pdf)

### Inferences
- Mapping, via sites as resources. Precompute for each dense footprint a set of legal via sites (dog-bone positions
  in the inter-pad gaps per Lattice/Altium patterns, and via-in-pad where the rules allow). Assign them to pads before
  routing, as PAO assigns access points. Treat unassigned sites in the window as reserved: high base cost for foreign
  nets, plus history growth in negotiation. This turns "a via was placed too close to the pins" from something seen
  by eye into a priced conflict.
- Mapping, keep-out cost field. A cheap approximation of the Bayless exclusion is a per-footprint cost field that is
  high on lattice nodes inside the escape window for vias of nets that do not belong to the footprint. It can be
  graded by channel criticality: higher on the last free channel between pad rows, lower at the window edge. This
  fits TraceMaker's existing cost-to-go and field machinery, and it is integer and deterministic.
- Mapping, via relocation as a repair move. No academic PCB via-relocation algorithm was found. A natural repair is a
  local, transaction-scoped move: when a pin's escape fails and the blocker is a via of net N, try moving that via
  within its own net's slack (re-route N's two stubs to an alternate via site outside the window). Commit only if the
  board score improves, which matches TraceMaker's branch/commit rule. This is an engineering proposal, not a
  published method.
- The 39% via reduction from simultaneous escape (Ozdal et al. 2008) says much of the via count on dense boards comes
  from crossings created by escaping components independently. Planning escape order across facing components
  removes vias at the source.

### Gaps
- No peer-reviewed paper found on post-route via relocation, via pushing, or via shoving for PCBs, and none measuring
  the effect of via keep-out fields around pads on completion. Freerouting/Specctra-style push-and-shove is
  documented in software, not in papers found here.
- No academic paper found on automated dog-bone via assignment for BGA fan-out; only patents and vendor guides.
- The VLSI "via pillar" and redundant-via literature was not searched within the tool budget.

## 4. Quantitative evidence that pre-planned fan-out or access reservation raises completion

### Takeaway
The evidence is real but narrow. It is almost all measured on BGA-only escape or on VLSI pin access, as layer count
or failed-pin count, not full-board completion. Joint via-and-trace escape (SAT) saves 1-2 signal layers on 13+ of 23
commercial BGAs. Coordinated pin-access planning takes failed pins from thousands to 0 and final DRCs from 755 to 2 on
ISPD-2018. Outside-routability-aware escape cuts layers about 17%. Simultaneous escape cuts vias about 39%. Negotiated
escape matches Allegro (7/14 complete each). No paper found reports full-board completion on hobby or industrial
KiCad-style boards from a fan-out pre-pass.

### Cited Findings
- SAT joint via/trace escape against greedy layer-by-layer: fewer layers on 13/23 BGAs (blind vias), up to 2 fewer
  (through vias, e.g. TI TMS320C 3 vs 5), never more; minimal layer count proven on several parts — [Bayless et al. 2016, Tables 1-2](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)
- Pin-access oracle: failed pins 31-7,816 down to 0 on all 10 ISPD-2018 designs; final routed DRCs 2 against 755
  (Dr. CU 2.0) on ispd18_test5 — [Tao of PAO](https://vlsicad.ucsd.edu/Publications/Conferences/377/c377.pdf)
- Outside-routability-aware escape for MCM pin clusters: about 17% layer reduction on average (abstract) — [HKBU record](https://scholars.hkbu.edu.hk/en/publications/optimal-routing-algorithms-for-rectilinear-pin-clusters-in-high-d/)
- Simultaneous escape for via minimisation: about 39% fewer vias on industrial cases (abstract) — [HKBU record](https://scholars.hkbu.edu.hk/en/publications/simultaneous-escape-routing-algorithms-for-via-minimization-of-hi/)
- NCER negotiated-congestion escape: 7 of 14 industrial cases fully routed, same as Cadence Allegro (abstract) — [ResearchGate](https://www.researchgate.net/publication/224132327_A_negotiated_congestion_based_router_for_simultaneous_escape_routing)
- Matching-based escape with real PCB rules: 100% routability, 0 DRC, on all industrial instances; two
  state-of-the-art routers failed (abstract; instance count not seen) — [ACM DL](https://dl.acm.org/doi/abs/10.1109/DAC56929.2023.10247781?download=true)
- MMCF OER with differential pairs and blockages: 100% routability on its test cases (abstract) — [ACM DL](https://dl.acm.org/doi/10.1145/3185783)
- Industry view: escape routing of dense BGAs is "often the single factor that forces extra layers to be added to
  PCBs" (Pfeil 2007, cited by Bayless et al.) — [Bayless et al. 2016](https://www.cs.ubc.ca/labs/isd/Projects/monosat/monosat_bga_iccad2016.pdf)

### Inferences
- In TraceMaker the layer count is fixed by the board. "Saves a layer" in these papers translates to "can complete on
  the given layers where the greedy method cannot". Bayless's greedy baseline, which routes the top layer to the max
  and then drops vias for the rest, is close to what a strict-first-pass lattice router does implicitly. That is
  evidence that a planned escape helps on exactly the boards where TraceMaker fails.
- PAO's before/after is the closest analogue to TraceMaker's failure mode (a): un-coordinated access leaves pins
  unreachable through no single bad decision, and coordination removes it.

### Gaps
- No study found measuring overall board completion (all nets) with and without a fan-out pre-pass on general PCBs.
- The benchmark sets (commercial BGA pinouts in Bayless; ISPD-2018; unnamed "industrial" sets elsewhere) are not
  public PCB boards. No paper found used PCBench, DAC 2020 PCB, or KiCad demo boards for escape studies.

## 5. Recent work (2018-2026), including learning-based and RL approaches

### Takeaway
Classical combinatorial methods dominate recent escape work: MMCF/LP OER (2018, 2024), matching-based escape with
real rules (DAC 2023, TCAD 2025), and hierarchical SER inside full PCB flows (DAC 2021). Learning-based escape is
immature: the one escape-specific RL paper found works on a 25x26 toy grid with at most 90 pins and does not compare
with flow or SAT methods. No evidence was found that learning beats classical escape methods.

### Cited Findings
- Cheng & Cheng, "Escape routing using deep reinforcement learning," SPIE vol. 13510, 2025. A DQN on an MDP
  formulation, tested on a simulated 25x26 grid with 30, 60 and 90 pins; generic escape, not BGA-specific (abstract) —
  [SPIE](https://www.spiedigitallibrary.org/conference-proceedings-of-spie/13510/135101B/Escape-routing-using-deep-reinforcement-learning/10.1117/12.3058004.short); [ADS](https://ui.adsabs.harvard.edu/abs/2025SPIE13510E..1BC/abstract)
- "GPCB Routing: Generative Pretrained Transformers-Based Printed Circuit Board Routing Method," IEEE TCAD 2024/2025
  (record only; method and results not seen) — [ACM DL](https://dl.acm.org/doi/abs/10.1109/TCAD.2024.3486241)
- Offline RL on top of iterative detailed-routing search to speed convergence (VLSI, arXiv 2512.03594). It cites Chen
  et al. combining GNNs with PPO for online RL routing guidance — [arXiv 2512.03594](https://arxiv.org/pdf/2512.03594)
- Vendor material (DeepPCB) says RL learns BGA breakout heuristics such as via minimisation and dog-bone versus
  via-in-pad choices, with no published benchmark; treat as unverified — [DeepPCB blog](https://deeppcb.ai/reinforcement-learning-pcb-routing-explained/)
- Classical recent work: DAC 2021 complete PCB methodology with concurrent hierarchical SER ([NYCU](https://scholar.nycu.edu.tw/en/publications/a-complete-pcb-routing-methodology-with-concurrent-hierarchical-r/));
  DAC 2023 / TCAD 2025 matching-based escape with variable rules ([ACM DL](https://dl.acm.org/doi/abs/10.1109/DAC56929.2023.10247781?download=true));
  2024 two-stage LP + heuristic OER ([ScienceDirect](https://www.sciencedirect.com/science/article/abs/pii/S0167926024001342));
  SPIE 2023 SER on multi-commodity flow ([SPIE 12610](https://biomedicaloptics.spiedigitallibrary.org/conference-proceedings-of-spie/12610/126102C/Simultaneous-escape-routing-based-on-multi-commodity-flow/10.1117/12.2671161.full));
  PCB routing on unstructured meshes with conflict-based search (J. Supercomputing 2025) ([Springer](https://link.springer.com/article/10.1007/s11227-025-07569-0))
- In VLSI, learned pin-accessibility prediction is used to guide placement, not to replace the access oracle, and
  ML-for-P&R surveys position learning as guidance around classical routers — [ML P&R survey arXiv 2202.13564](https://arxiv.org/pdf/2202.13564)

### Inferences
- For TraceMaker, the recent literature supports adding a classical, deterministic escape/access planner (flow or
  chain DP plus reservation) over a learned one. It is also easier to keep within TraceMaker's rules: determinism,
  a reference path, and exact geometric checks.
- Constraint-solver escape (Bayless; MonoSAT is open source) could serve as an offline oracle on failing footprints,
  to show whether a fan-out is feasible on the board's layers at all. That would separate "router weakness" from
  "geometrically impossible" among the 335 package-fan-out opens.

### Gaps
- No 2018-2026 learning-based escape or fan-out paper with results on real BGAs or PCBs was found. The GPCB paper's
  results were not reached.
- Conflict-based-search PCB routing (2025) was seen only as a title; its completion claims are unknown.
