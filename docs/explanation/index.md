---
status: current
code: []
---

# Explanation

The design documents: what TraceMaker is meant to do, how it does it, and why, with the papers each algorithm comes
from cited where it is defined. They are the source of truth for the design: when the code has to differ, the
document changes in the same pull request, with a [decision record](../12-decisions.md).

Read them in this order the first time:

| Document | What it explains |
|---|---|
| [Requirements](../01-requirements.md) | What the product must do, its scope, success criteria and glossary |
| [Architecture](../02-architecture.md) | Processes, the pipeline, threading, determinism, the repository layout |
| [Data structures and DRC](../03-data-structures.md) | Geometry, spatial indices, search structures, the exact design-rule check |
| [Placement](../04-placement.md) | Optimality levels; analytic placement, legalisation, annealing, exact windows, ECO moves |
| [Routing](../05-routing.md) | The routing stages, the escalation ladder, the cost model, and every routing feature since |
| [Failure memory](../06-failure-learning.md) | Learning from failed attempts: records, nogoods, history, bandits, the knowledge base |
| [GPU acceleration](../07-gpu.md) | What runs on the GPU, the kernels, and how CPU and GPU give identical results |
| [KiCad integration](../08-kicad-integration.md) | File I/O, `kicad-cli`, the plugin, the DRC judge |
| [Live viewer](../09-visualization.md) | Streaming, replay and the visual design of the browser viewer |
| [Benchmarking](../10-benchmarking.md) | Datasets, metrics, baselines, the harness and the gates |

Studies and features with a document of their own:

| Document | Status |
|---|---|
| [Placement test plan](../14-placement-test-plan.md) | How placement is tested: levels, datasets, metrics, gates |
| [Component-aware rules](../15-component-rules.md) | Partly built (M13 P0–P3): detect parts such as USB, crystals and regulators, and apply their layout rules |
| [BGA breakout](../16-bga-breakout-plan.md) | Partly built (M14, M15): matching tscircuit on its BGA breakout sets, with measured outcomes |
| [Advice files and advisors](../17-llm-advisor.md) | Partly built (M16, stages 0–3): hints for the router when it is stuck; the engine never talks to a model |
| [Agent interface](../19-agent-interface.md) | Partly built (M18, stages A–B): a board session an agent drives over MCP or JSON lines |

Background, outside this site: the routing-stage design and the Freerouting evidence it rests on
([`cpp_autorouter_clean_sheet.md`](../../cpp_autorouter_clean_sheet.md)), a survey of placement and routing
approaches ([`pcb-routing-optimization.md`](../../pcb-routing-optimization.md)), and verified facts on KiCad,
its APIs, benchmark sets and GPU techniques ([`research/`](../../research/)).
