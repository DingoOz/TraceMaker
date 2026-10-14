# M15 (srj20 push, started 2026-10-11 ~20:30 UTC) — assumptions and decisions taken without asking

The user asked for six hours of research and experiments on the srj20 boards, with no stopping for questions. Where an answer was
needed, an assumption was made and is listed here.

- A1. Work happens on branch `m15-srj20` (from `main` at `c6fcb7b`). Nothing is pushed or merged until asked.
- A2. Experiments use the same protocol as M14 so the numbers compare: KiCad DRC as judge, `--no-gpu`, one thread per board, work budgets
  (`--work`), `--fast-bends`, automatic breakout settings, no knowledge base. Gap sets: `srj20-gap` (57 boards), `srj20-gap2` (35).
- A3. The success measure is clean boards of the full srj20 set (target 157/200; M14 ended at 133). A change counts only if it also
  leaves `quick` (30) and `mid` (11) byte-identical, or measurably no worse, and `ctest` passes without a GPU.
- A4. "Matched effort" with tscircuit (360 s, 32 vCPU) is read as: at most 360 s wall per board on at most 32 threads. A 16-variant portfolio
  on 16 threads is inside that.
- A5. Pitch / lattice changes and other geometry-level defaults are allowed to become automatic only on boards the breakout rule (D98) selects.
- A6. Hidden option flags are kept for experiments that did not help (D94 practice) and recorded in docs/16 §12.
- A7. The machine is shared by background benchmark jobs of this session (up to ~48 cores); runs are work-budgeted so results do not depend on load.
