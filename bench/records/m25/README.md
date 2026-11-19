# M25: one recorded default route for the explanation page

The numbers quoted in [docs/explanation/default-route.md](../../../docs/explanation/default-route.md) come from this run.

- **Board**: `tests/boards/plane_smd/plane_smd.kicad_pcb` (TraceMaker's own test board: four copper layers, filled
  plane zones, 11 nets, 47 connections; one net class, 0.1 mm track and clearance). Input SHA-256 starts `8997b344e21c5379`.
- **Engine**: `build/release/src/app/tracemaker` (0.12.1) in the worktree of branch `m26-impl` at `9b77616`. Route code
  (`src/route`, `src/app`) as committed there.
- **Machine**: 56 cores, 8 threads used, RTX 5060 Ti; 2026-11-17.
- **Command**: [`command.sh`](command.sh). Default options plus `--work 3000000 --no-kb`, so the result does not depend
  on the machine or on earlier runs, and `--json`, `--report`, `--view` (they only observe).

| File | What it is |
|---|---|
| `route.log` | The run's standard error: per-variant search counters, the portfolio table, the rescue passes, the summary, the why-open proof |
| `route.json` | `--json` summary (`/variants`, `/rescue`, `/why_open`, `/stuck`, `/drc`, `/critical_nets`, `/work_all_variants`) |
| `report.md` | `--report` run report, rendered from `route.json` alone (D136) |

## Result

| | Value |
|---|--:|
| Connections | 47 |
| Routed | 41 (87.2 %) |
| Tracks / vias added | 223 / 0 |
| Kept result | plain portfolio, variant 5 (cheap vias, half pitch on a small board) |
| Lattice pitch of the kept variant | 0.025 mm (0.030 mm in the other variants) |
| Relax rescue | warm 41 -> 39, fresh 41 -> 39; neither kept |
| Why open (11 pins) | 10 open through congestion, 1 walled in only by zone fills |
| DRC errors added | 0 |
| Wall time | 13.2 s (whole job), 3.98 s for the kept variant |
| Exit code | 3 (connections left open) |

Plain portfolio, routed per variant: 39, 41, 36, 40, 36, **41**, 40, 41. Variants 1, 5 and 7 tie at 41; variant 5
wins the tie-break (fewer vias, then shorter copper; the run does not record the losers' vias).

Repeatability: the same command run again gave a byte-identical routed board (SHA-256 starts `dc1d1073de5b35f6`).
The routed board is not kept here; `command.sh` makes it again.
