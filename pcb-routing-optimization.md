# PCB Routing and Component Placement: Optimization Approaches

Notes from a conversation on designing PCB routing software that also includes component placement: optimization methods, the state of the art, open-source implementations, and tscircuit.

## Optimization approaches

### Placement

**Simulated annealing** is the workhorse. The OpenROAD SA-PCB tool is a useful reference implementation. Components are moved randomly; worse solutions are accepted early to escape local minima, then the temperature cools. Pair it with a cost function built on half-perimeter wirelength (HPWL) plus a routability estimate, and you get placements that actually route.

**Genetic algorithms** are the other major family. Cadence’s Allegro X AI team applied a multi-objective GA to two-sided boards, optimizing wirelength and routability together. The related MIT thesis (Thomas Ngô, 2024, *Application of Multi-Objective Genetic Optimization in PCB Component Placement*) is worth reading.

### Routing

The classic baseline is **A\*** on a grid. That is what FreeRouting does, with rip-up-and-retry when nets block each other. FreeRouting actually operates on a continuous geometric plane rather than a pure cell maze, but the search core is still A\*:

\[
f(n) = g(n) + h(n)
\]

where \(g(n)\) includes trace length, vias, preferred-direction penalties, and rip-up costs.

The newer classical direction is **negotiation-based routing** such as PathFinder. Every edge is a shared resource; the cost of congested areas rises each iteration, and the worst offenders are ripped up and re-routed. OrthoRoute uses that on a GPU and handles boards with thousands of airwires (Manhattan lattice, horizontal/vertical layer pairs, blind and buried vias).

### State of the art

The current frontier is **reinforcement learning**:

- **DeepPCB** claims median 100% completion across tens of thousands of real boards (about 32,000–40,000 jobs), with roughly 86–87% of boards hitting 95%+ completion. Commercial, RL-based, multi-format (Altium, KiCad, OrCAD, EAGLE, Zuken, EasyEDA).
- **DreamerV3+FR** (2026) wraps FreeRouting in a model-based RL world model. Reported ~96% completion, 21% less training time than DQN, and generalization to 6-layer boards where A\*, DQN, PPO, and generative/LLM methods failed. Code is public.
- **3D LineExplore** (Scientific Reports, Jan 2026) is a gridless geometric method: radar-style scanning, obstacle-avoidance heuristics, multi-pin repair. Reported >98% success and shorter wire length than FreeRouting, ELECTRA, DeepPCB, and Optimized-3D-A\* on public benchmarks. Paper only; data available on request, no public code release found.
- **Quilter** frames a third era: generative, physics-anchored full-board candidates rather than sequential geometric autorouting.
- **tscircuit-autorouter** uses successive approximation and hypergraphs rather than sequential net-by-net routing.
- LLM agents on the OmniRouting benchmark remain far below human and classical routers (best zero-shot net routing rate around 13% vs ~94% human).

A practical design is a two-stage pipeline: anneal the placement with a routability-aware cost, then route with a negotiation or RL engine. The hard part is coupling them so placement decisions reflect what the router can actually do.

## Open-source implementations

| Approach | Project | License | Notes |
| --- | --- | --- | --- |
| A\* / rip-up-and-retry | [FreeRouting](https://github.com/freerouting/freerouting) | GPL-3.0 | Specctra DSN/SES. Backbone of DreamerV3+FR. KiCad via DSN export/import. |
| World-model RL on FreeRouting | [dreamer-Autorouting](https://github.com/yinqimakeitfun/dreamer-Autorouting) | (see repo) | JPype wrapper of `freerouting.jar` as a Gymnasium env, trained with DreamerV3. |
| PathFinder, GPU | [OrthoRoute](https://github.com/bbenchoff/OrthoRoute) | MIT | KiCad plugin. Manhattan lattice. Strong on huge regular backplanes; weaker as a general router (PCBWorld: ~1–2% clean-pass on mixed boards). |
| A\*, Rust core | [KiCadRoutingTools](https://github.com/drandyhaas/KiCadRoutingTools) | MIT | KiCad 9/10. Diff pairs, length matching, plane pours, placement quench / route-in-the-loop. |
| Hypergraph pipeline | [tscircuit-autorouter](https://github.com/tscircuit/tscircuit-autorouter) | MIT | Built-in autorouter for tscircuit. TypeScript. |
| RL environment on KiCad | [PCBWorld](https://github.com/LGAI-Research/PCBWorld) | BSD-3-Clause (engine GPLv3) | Gymnasium env over KiCad’s push-and-shove router. PPO/GRPO baselines. |
| RL placement | [RL_PCB](https://github.com/LukeVassallo/RL_PCB) | (see repo) | Learning-based component placement (DATE 2024). |
| Simulated annealing placement | [SA-PCB](https://github.com/The-OpenROAD-Project/SA-PCB) | BSD-3-Clause | OpenROAD PCB annealer. Archived July 2026 (read-only). HPWL, polygon overlap, 90/45° rotation, Timberwolf-style cooling. |

**Not open source (or no public code found):**

- DeepPCB — commercial.
- Cadence Allegro X AI multi-objective GA — paper/thesis only.
- 3D LineExplore — paper only; code not released.
- Quilter — commercial generative router.

A realistic open-source stack is FreeRouting, KiCadRoutingTools, or tscircuit-autorouter for routing. Placement is thinner: SA-PCB’s source is a usable template for writing your own annealer.

## What is tscircuit?

tscircuit is an open-source toolchain for designing real PCBs with React and TypeScript — “React for electronics.”

Instead of dragging footprints in a GUI, you write code: a board element, chips, resistors, traces between pins. That compiles to **Circuit JSON**, an intermediate format, then to:

- schematic and PCB previews
- 3D models (glTF/GLB/STEP)
- Gerbers, pick-and-place, BOM
- Specctra DSN (for FreeRouting)
- KiCad schematic, PCB, project zip, and library export

It has a CLI (`tsci`), an online playground, a component registry, and works with ordinary TypeScript tooling and AI coding assistants. The autorouter above is its built-in MIT-licensed engine. It can also hand off to FreeRouting via DSN.

It is a different workflow from KiCad — code-first, version-controllable, runnable in the browser — and still maturing relative to the large commercial EDA suites.

- Docs: https://docs.tscircuit.com/
- Site: https://tscircuit.com
- Repo: https://github.com/tscircuit/tscircuit

## Suggested reading / repos

- FreeRouting: https://github.com/freerouting/freerouting
- OrthoRoute: https://github.com/bbenchoff/OrthoRoute
- KiCadRoutingTools: https://github.com/drandyhaas/KiCadRoutingTools
- tscircuit-autorouter: https://github.com/tscircuit/tscircuit-autorouter
- DreamerV3+FR: https://github.com/yinqimakeitfun/dreamer-Autorouting
- PCBWorld: https://github.com/LGAI-Research/PCBWorld
- SA-PCB (archived): https://github.com/The-OpenROAD-Project/SA-PCB
- 3D LineExplore paper: https://doi.org/10.1038/s41598-026-36925-0
