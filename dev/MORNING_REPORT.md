# Overnight report — 2026-10-02 (11:04 → 19:00 UTC)

## Results first

TraceMaker routes real KiCad boards and is judged by KiCad's own DRC (`kicad-cli pcb drc`), against Freerouting's
own published results on the same PCBench fixtures:

| Tier (PCBench, Freerouting's tiers) | Boards | TraceMaker clean pass | Freerouting 2.5.0-RC12 | Freerouting 2.4.1 | Added DRC errors |
|---|--:|--:|--:|--:|--:|
| A — routine 2-layer | 40 | **100%** | 100% | 87.5% | 0 boards |
| B — 2–4 layer | 40 | __B__ | __BFR__ | __BFR241__ | __BADD__ |
| C — complex / multi-layer | 30 | __C__ | __CFR__ | __CFR241__ | __CADD__ |

- Clean pass = fully connected **and** zero router-introduced KiCad DRC errors. 120 s per board, 8 router variants
  in parallel per board. Tier-A median time 3 s (Freerouting's published median 30 s on its own hardware).
- Every run is on the progress site's benchmark panel: `http://<host>:8765/` (history, Freerouting columns).

## What was built (by milestone)

| Milestone | State | Highlights |
|---|---|---|
| M0 toolchain | done | CMake presets (release/cpu-only/asan/tsan), CUDA 12.4 + g++-13 host, Catch2, setup script, fixtures |
| M1 KiCad I/O | done | Lossless s-expression parser/editor; KiCad 4–10 boards; pcbnew-verified reader; 3,538 fixture boards round-trip byte for byte; rules (.kicad_pro/.kicad_dru/legacy); netlists via kicad-cli |
| M2 DRC | core done | KiCad-equivalent DRC; 11/16 KiCad demo boards match kicad-cli exactly on 15 routing-relevant types (regression test); rule semantics learned from KiCad (see decisions) |
| M3 viewer | done (v1) | WebGL2 viewer + Boost.Beast WebSocket server (helper agent); `tracemaker route --view` streams routing live (tracks, rip-ups, search frontiers, failures, ratsnest, stats) |
| M4 router | done | Octilinear lattice A* with exact legality from the DRC rule engine; exact verification before every commit; writes KiCad tracks/vias |
| M5 learning | mostly done | PathFinder history, negotiated rip-up, boxed-in detection, learned blocks, nogoods, escalation ladder (escapes → neck-down → negotiation), restarts that keep history |
| M6 GPU | partial | CUDA cost-to-go fields (GAMER-style sweeps) as A* heuristic; byte-identical results with/without GPU; global routing corridors not started |
| M7 placement | __PLACE__ | |
| M10 portfolio / determinism / KB | mostly done | 8-variant parallel portfolio; deterministic `--work` budget (byte-identical repeats); SQLite knowledge base with a Thompson-sampling bandit over variants and per-board failure priorities |

## How to look at it

- Progress site: `http://192.168.1.82:8765/` (roadmap, tests, latest code, benchmarks, GPUs, activity).
- Live routing: `build/release/src/app/tracemaker route <board> -o out.kicad_pcb --view --hold` → `http://192.168.1.82:8766/`.
- Routed benchmark boards: `bench/results/final-tier{A,B,C}/boards/*.kicad_pcb` (open in KiCad 10).
- Everything is committed locally (`git log`); nothing was pushed anywhere.

## Things worth knowing (found by testing against KiCad)

KiCad behaviours the router now honours, each found on a real board: board minimum clearance is a floor; local pad/
footprint clearance beats net classes; coupled diff pairs use min(clearance, gap); plated through-holes are on all
copper layers whatever the file says; overlapping pads of different footprints are not connected; footprint copper
graphics are net-less for routing; non-plated holes keep the board-edge clearance; pad solder-mask openings bridge on
either side; untented vias open the mask; copper text is an obstacle (multi-line, justified, mirrored); KiCad 5 arcs
and filled polygons; regex net-class patterns; KiCad caps each violation type at 199 reports.

## Decisions taken without you

All in `dev/assumptions.md` (A1–A16), each with its reason. The ones you may want to revisit:
- A1 local git commits under a repository-local identity (`dingo`, your account email); nothing pushed.
- A3 schematic connectivity via `kicad-cli` netlists rather than a native schematic resolver (for now).
- A11 DRC parity is "good enough to route against"; KiCad remains the judge.
- A16 benchmark "added errors" count router-introduced violations only (involving a track/via/arc).

## Known issues and next steps

1. Some pins stay "boxed in" on dense fine-pitch boards although the same net routes alone (avr_ledprojector):
   suspected interaction between the net-independent fixed-obstacle cache and neighbouring pads. Diagnostics:
   `TM_DEBUG_ENCLOSED=1`, `tracemaker debug-pad`, `--only-net`.
2. Edge-connector fingers with mask openings on both sides block their own exits (fifogfx_c64cart).
3. Copper text uses estimated glyph boxes (conservative); exact outlines from KiCad would free space.
4. Speed: the A* loop is the bottleneck on large boards; global routing corridors (M6) and bidirectional search
   are the next levers. GPU fields help modestly today.
5. KiCad IPC plugin (M11), schematic-to-board flow, escape planning (M9), diff pairs/length tuning (M12): not started.
