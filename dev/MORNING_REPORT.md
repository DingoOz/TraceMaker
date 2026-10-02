# Overnight report — 2026-10-02 (11:04 → 19:00 UTC)

## Results first

TraceMaker routes real KiCad boards. KiCad's own DRC (`kicad-cli pcb drc`) judges every result, and the comparison
is against Freerouting's published results on the same PCBench fixtures (Freerouting's own benchmark set and tiers):

| Tier (PCBench) | Boards | TraceMaker clean pass | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 | Boards with router-introduced DRC errors |
|---|--:|--:|--:|--:|--:|
| A — routine 2-layer | 40 | **100%** | 100% | 87.5% | 0 |
| B — 2–4 layer | 40 | **62.5%** | 50.0% | 12.5% | 0 |
| C — complex / multi-layer | 30 | **56.7%** | 46.7% | 16.7% | 0 |
| D — hardest | 21 | **52.4%** | 38.1% | 0.0% | 0 |

- **Clean pass** means fully connected and zero router-introduced KiCad DRC errors. Each board gets 120 s, with
  8 router variants in parallel.
- **Runs:** all four tiers are from the last runs of the night, `final7-tier{A,B,C,D}`, on the latest router.
- **Samples:** 40 of 453 tier-A boards, 40 of 560 tier-B boards, 30 of 122 tier-C boards and the 21 tier-D boards
  that have a KiCad file.
- **Speed:** the tier-A median is 3 s per board. Freerouting's published median is 30 s on its own hardware.
- **Variance:** results vary a little between runs because of the wall-clock limit. Earlier runs of the same day
  scored B 50–60% and C 33–60%. A deterministic `--work` budget gives byte-identical repeats when needed.
- **Head to head on tier B:** both clean on 17 boards, only TraceMaker on 8, only Freerouting on 3.
- **Head to head on tier C:** both clean on 12 boards, only TraceMaker on 5, only Freerouting on 2.
- **Head to head on tier D:** both clean on 6 boards, only TraceMaker on 5, only Freerouting on 2.
- All runs are on the progress site's benchmark panel with Freerouting columns.

## What was built

| Milestone | State | Highlights |
|---|---|---|
| M0 toolchain | done | CMake presets (release, cpu-only, asan, tsan); CUDA 12.4 with g++-13 host; Catch2; setup script; fixtures |
| M1 KiCad I/O | done | Lossless s-expression parser and editor; KiCad 4–10 boards; 3,538 fixture boards round-trip byte for byte; rules from .kicad_pro, .kicad_dru and legacy files |
| M2 DRC | core done | KiCad-equivalent DRC; 11 of 16 KiCad demo boards match kicad-cli exactly on 15 routing-relevant types (regression test) |
| M3 viewer | done (v1) | WebGL2 viewer and WebSocket server; `tracemaker route --view` streams routing live |
| M4 router | done | Octilinear lattice A* with exact legality from the DRC rule engine; exact verification before every commit |
| M5 learning | mostly done | PathFinder history; negotiated rip-up; boxed-in detection at both ends; learned blocks; nogoods; escalation (escapes, neck-down, negotiation); restarts that keep history |
| M6 GPU | partial | CUDA cost-to-go fields as the A* heuristic, identical results with or without GPU; global routing corridors not started |
| M7 placement | v1 done | `tracemaker-place`: quadratic placement, SimPL spreading, legalisation, parallel annealing, LP lower bound; `--mode auto --route-check` keeps the most routable of refine, full and input |
| M10 portfolio | mostly done | 8-variant parallel portfolio with early stop; deterministic `--work` budget; SQLite knowledge base with a bandit over variants and per-board failure priorities |
| M11 KiCad plugin | v1 done | KiCad 10 IPC action plugin routes the open board in one undoable commit (tested offline against kicad-python 0.8) |

## What changed this afternoon

- **Horticulture solder-mask bridge fixed.** KiCad grows mask graphics by the board mask expansion. The router now
  does the same, and the board routes clean (decision A18).
- **Boxed-in targets detected.** Only the source pad used to be tested for being boxed in. A short reverse search
  now detects a boxed-in target, so the escalation to escapes and neck-down runs. APM-RPi-Shield went from
  124 to 125 of 125 in direct runs.
- **Fine-pitch pad approaches.** A failing leg into a pad is dropped when the track already ends on the pad's
  copper. Blocks are now learned beside fine-pitch pads and for diagonal steps that clip a pad corner.
  USBI2C01 went from 72 to 73 of 73 at a fixed budget.
- **Portfolio early stop.** All variants stop once one routes every connection.
- **Faster routed-copper checks.** A count raster skips most routed-copper queries. Output is identical and runs
  are 4–10% faster.
- **Coarse lattice on large boards.** Two portfolio variants use double the lattice pitch on boards with at least
  3M lattice points per layer. P8000 went from 325 to 334 of 361 connections in 120 s.
- **Auto placement.** `--mode auto` picks between refine, full and the input placement by routing each.
- **More restarts.** When budget remains after the planned restarts, the router keeps restarting with variation
  instead of stopping early.
- **GPU field cut-off.** A variant stops building GPU fields when they exceed 30% of its wall-clock time, which
  happens when many jobs share the GPUs.
- **Copper text margin.** Text boxes grew by 15% of the text height after one clearance error near copper text
  on USBI2C01 in run final6. Run final7 has no router-introduced errors.
- **Placement integrated.** The placement agent's engine is committed. I added the router-in-the-loop check.

## Placement status (M7)

- `tracemaker-place --mode auto --route-check N` runs refine and full placement. It routes each against the input
  with the same deterministic budget and keeps the placement with the fewest unrouted connections, then the
  shortest wirelength.
- On 23 PCBench boards at a 3M-expansion budget it kept a new placement on 18 boards and the input on 5.
  Unrouted connections never went up. They went down on 2 boards (61 to 59 in total).

| Measure | Input placement | Kept placement |
|---|--:|--:|
| Total HPWL (mm) | 17,464 | 13,190 |
| Median HPWL ratio | — | 0.83 |
| Unrouted connections | 61 | 59 |

- Without the route check, placement alone cut wirelength but routed slightly worse (13 fully routed boards
  against 15). The routability term inside the annealer (stage G) and side flipping are the next steps.
- Per-board table: `build/place-auto/summary.json`; design notes in `docs/04-placement.md` section 7.

## How to look at it

- Progress site: `http://192.168.1.82:8765/` (roadmap, tests, latest code, benchmarks, GPUs, activity).
- Live routing: `build/release/src/app/tracemaker route <board> -o out.kicad_pcb --view --hold`, then open
  `http://192.168.1.82:8766/`.
- Placement: `build/release/src/place/tracemaker-place in.kicad_pcb -o out.kicad_pcb --mode auto --route-check 3000000`.
- Routed benchmark boards: `bench/results/final7-tier{A,B,C,D}/boards/*.kicad_pcb` (open in KiCad 10).
- Everything is committed locally (`git log`). Nothing was pushed anywhere.

## Decisions taken without you

All are in `dev/assumptions.md` (A1–A20) and `docs/12-decisions.md` (D13–D16 added tonight), each with its reason. The ones you may want to revisit:

- **A1:** local git commits under a repository-local identity (`dingo`, your account email); nothing pushed.
- **A3:** schematic connectivity comes from `kicad-cli` netlists rather than a native schematic resolver.
- **A11:** DRC parity is "good enough to route against"; KiCad remains the judge.
- **A16:** benchmark "added errors" count only violations involving a track, via or arc.
- **A17:** edge-connector fingers stay unrouted rather than create mask bridges. TraceMaker's clean pass is
  therefore stricter than Freerouting's figure.
- **A18:** mask graphics are grown by the board's mask expansion, inferred from kicad-cli behaviour.
- **A19:** copper text is modelled as a generous box. Exact glyph outlines would free some space.
- **Placement agent:** full mode aims for a 0.25 mm courtyard gap and falls back to KiCad's default of 0 on dense
  boards. Connectors near the edge stay fixed unless `--move-connectors` is given.

## Known issues and next steps

1. **Large boards are time-limited.** P8000 and PCIE-to-MXM finish only one or two passes in 120 s. A* costs about 430 ns
   per expansion. Global routing corridors, a coarse-to-fine lattice and parallel routing inside one variant are
   the next levers.
2. **Edge-connector fingers** with mask openings on both sides block their own exits (fifogfx_c64cart).
3. **Copper text** uses estimated glyph boxes, which is conservative.
4. **Board reader naming:** unplated-hole pad numbers and `~{...}` escaped references differ from pcbnew's naming
   in the parity comparison. Routing is unaffected.
5. **Not started:** schematic-to-board flow, escape planning (M9), diff-pair and length tuning (M12), Python
   bindings.
6. **Diagnostics for the next session:** `TM_DEBUG_ENCLOSED=1`, `TM_DEBUG_EXACT=1`, and the hidden subcommands
   `tracemaker debug-pad` and `tracemaker debug-seg`.
