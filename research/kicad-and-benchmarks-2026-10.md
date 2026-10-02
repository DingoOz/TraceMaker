# Research: KiCad automation and PCB routing benchmarks (checked 2026-10-02)

Gathered by direct page fetches and the GitHub/GitLab/PyPI APIs. Items marked **unverified** could not be confirmed.

## A. KiCad

| Item | Finding | Source |
|---|---|---|
| Latest stable | **10.0.6** (2026-08-29); 10.0.7-rc1 2026-09-27; 10.0.5 2026-07-22; 10.0.4 2026-06-21 | [1] |
| Major releases | 10.0.0: 2026-03-20. 9.0.0: 2025-02-20 | [2][3] |
| Board format | `SEXPR_BOARD_FILE_VERSION` **20260206** on 10.0; 20260901 on master (KiCad 11 dev) | [4] |
| Schematic format | `SEXPR_SCHEMATIC_FILE_VERSION` **20260306** on 10.0; 20260830 on master | [4] |
| IPC transport | Protobuf over NNG on UNIX sockets (named pipes on Windows); stabilised for 9.0 | [5] |
| Python client | `kicad-python` (import `kipy`) **0.8.0** (2026-08-30), Python ≥ 3.9, MIT-style | [6][7] |
| SWIG `pcbnew` | Deprecated since 9.0; planned removal in **KiCad 11** | [8] |
| Headless IPC | Headless server via `kicad-cli` is a **KiCad 11** feature; 9/10 need a running GUI. kipy master has `server.py` helpers | [9][7] |
| Plugin packaging | `plugin.json` (schema go.kicad.org/api/schemas/v1) in `${KICAD_DOCUMENTS_HOME}/<ver>/plugins/<dir>`; Python plugins get a per-plugin venv; KiCad passes `KICAD_API_SOCKET`, `KICAD_API_TOKEN`. Whether the API server must be enabled in Preferences first is unverified | [9] |
| Docker | `kicad/kicad` tags `10.0`, `10.0.6`, `10.0-full`, `10.0.6-full`, `-amd64` (updated 2026-09-22) | [10] |

### IPC operations (10.0 `.proto` files) [11]

| Need | 10.0 | Added on master (11) |
|---|---|---|
| Read | `GetItems`, `GetItemsById`, `GetItemsByNet`, `GetItemsByNetClass`, `GetConnectedItems`, `GetBoundingBox`, `HitTest`, `GetPadShapeAsPolygon`, `CheckPadstackPresenceOnLayers`, `GetBoardStackup`, `GetBoardEnabledLayers` | `GetBoardBoundingBox` |
| Create/modify/delete | `CreateItems`, `UpdateItems`, `DeleteItems` (move/rotate footprint = update position/orientation) | `PlaceFootprintFromLibrary` |
| Flip | `FlipItems` | |
| Interactive move | `InteractiveMoveItems` | |
| Nets/classes | `GetNets`, `GetNetClassForNets`, `GetNetClasses`/`SetNetClasses` | `Get/SetNetClassAssignments`, `ImportNetlist` |
| Design rules | — | `Get/SetBoardDesignRules`, `Get/SetCustomDesignRules` |
| Commit/undo | `BeginCommit`/`EndCommit` (kipy `begin_commit`, `push_commit`, `drop_commit`) | |
| Zones | `RefillZones` | |
| DRC | **No run-DRC command** (only `InjectDrcError`); use `kicad-cli pcb drc` | |
| Save | `SaveDocument`, `SaveCopyOfDocument`, `SaveDocumentToString`, `RevertDocument`, `RunAction` | export jobs |

### kicad-cli (10.0) [12]

`pcb drc`: `--output`, `--define-var`, `--format report|json`, `--all-track-errors`, `--schematic-parity`, `--units`,
`--severity-all|error|warning|exclusions`, `--exit-code-violations` (0 clean, 5 violations), `--refill-zones`, `--save-board`.

`sch export netlist --format`: `kicadsexpr` (default), `kicadxml`, `cadstar`, `orcadpcb2`, `spice`, `spicemodel`, `pads`, `allegro`; `--variant`.

No `kicad-cli` Specctra DSN export in 10.0 (Freerouting uses SWIG `pcbnew.ExportSpecctraDSN`, removed in 11) [12][13].

### Custom DRC constraint keywords (10.0 parser) [14]

`annular_width, assertion, bridged_mask, clearance, connection_width, courtyard_clearance, creepage, diff_pair_gap,
diff_pair_uncoupled, disallow, edge_clearance, hole_clearance, hole_size, hole_to_hole, length, min_resolved_spokes,
physical_clearance, physical_hole_clearance, silk_clearance, skew, solder_mask_expansion, solder_paste_abs_margin,
solder_paste_rel_margin, text_height, text_thickness, thermal_relief_gap, thermal_spoke_width, track_angle,
track_segment_length, track_width, via_count, via_dangling, via_diameter, zone_connection` (values `min/opt/max`).
`disallow` types: `track, via, micro_via, blind_via, buried_via, through_via, pad, zone, text, graphic, hole, footprint`.
Other keywords: `severity`, `layer`, `condition`, `assign_component_class`.

## B. Benchmarks

| Set | URL | Boards | Formats | Licence / notes |
|---|---|---|---|---|
| Freerouting `fixtures/` | github.com/freerouting/freerouting/tree/master/fixtures | 155 `.dsn` (mostly `IssueNNN-*`), plus `Issue508-DAC2020/` (bm01–bm11 with FR SES, logs, KiCad DRC reports) | DSN, SES | GPL-3.0 |
| Freerouting benchmark harness | `scripts/benchmark/`, `scripts/pcbench/` | `DAC2020_boards` (10 DSN, no bm03), `KiCad_10_demos` (14 entries, 10 DSN), `PCBench` (1,158 folders with unrouted/reference DSN, `.kicad_pcb`, `ground_truth.json`); `metadata.yaml` with size/layers/nets/timeouts/tiers | DSN, kicad_pcb, JSON | Publishes scripts + nightly results (`results/benchmarks.md`, 2026-09-30). **v2.5.0-RC12 on 1,157 fixtures: 74.6% clean (0 DRC), 74.8% fully routed**; v1.9.0: 35.9% / 49.1%. KiCad-DRC scoring (`DrcRunner.ps1`) |
| KiCad `demos/` (10.0) | gitlab.com/kicad/code/kicad/-/tree/10.0/demos | 19 `.kicad_pcb` (cm5_minima, complex_hierarchy, ecc83-pp ×2, interf_u, jetson-agx-thor-baseboard, kit-dev-coldfire-xilinx_5213, microwave, multichannel_mixer routed+unrouted, One-Air-Max, pic_programmer, RoyalBlue54L-Feather + NFC antenna, sonde xilinx, StickHub, tinytapeout-demo, video, vme-wren) | kicad_pcb | KiCad GPL-3.0+; per-board licence unverified |
| DAC 2020 bm1–bm11 | github.com/DAC-2020-Submission-1703/PCB-Benchmarks | 11 hand-routed manufactured designs (zones removed, some parts locked) | `bmN.routed.kicad_pcb` | No licence file; anonymised. Metrics: through vias, routing layers, laser-via layers, bbox area, track length. "ASP-DAC 2021" origin unverified |
| FanoutNet (AAAI 2023) | — | unverified | — | — |
| PCBench | github.com/PCBench/PCBench | README says 164; repo has ~1,182 `PCBs/*` folders (`raw.kicad_pcb`, `processed.kicad_pcb`, `final.json`, `metadata.json`, `visual.png`) | kicad_pcb, JSON | MIT; last commit 2024-01-18 |
| PCBWorld (arXiv 2607.05915) | arxiv.org/abs/2607.05915; github.com/LGAI-Research/PCBWorld | Song et al., LG AI Research (v1 2026-07-07, v2 2026-09-11). Two synthetic generators + **679 real boards**; D3 derived from PCBench; 8 KiCad-checked metrics | kicad_pcb | Code BSD-3; engine GPLv3, pins KiCad 9.0.8 |
| PCB-Bench (ICLR 2026) | github.com/digailab/PCB-Bench | LLM reasoning QA, 174 OSHWHub projects | mixed | Not a routing benchmark |
| OmniLayout / OmniRouting | arXiv 2607.03261 / 2608.04434 | 1,681 designs | unverified | unverified |
| RL_PCB (Vassallo & Bajada, DATE 2024) | github.com/LukeVassallo/RL_PCB | 6 training circuits; eval size unverified | kicad_pcb | MIT |
| DeepPCB (tangsanli5201) | — | Defect-detection images — **not relevant** | | |

Seen but not checked: ersanyichen/Parsed-PCB-Benchmark, logicia32/router-arena (MIT), arXiv 2210.14259 (14 placement designs).

## References
1. https://www.kicad.org/blog/
2. https://www.kicad.org/blog/2026/03/Version-10.0.0-Released/
3. https://www.kicad.org/blog/2025/02/Version-9.0.0-Released/
4. gitlab.com/kicad/code/kicad: `pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr.h`, `eeschema/sch_file_versions.h`
5. https://dev-docs.kicad.org/en/apis-and-binding/ipc-api/
6. https://pypi.org/project/kicad-python/
7. https://gitlab.com/kicad/code/kicad-python
8. https://dev-docs.kicad.org/en/apis-and-binding/pcbnew/
9. https://dev-docs.kicad.org/en/apis-and-binding/ipc-api/for-addon-developers/
10. https://hub.docker.com/r/kicad/kicad
11. https://gitlab.com/kicad/code/kicad/-/tree/10.0/api/proto
12. https://docs.kicad.org/10.0/en/cli/cli.html
13. https://github.com/freerouting/freerouting/blob/master/scripts/pcbench/README.md
14. https://gitlab.com/kicad/code/kicad/-/blob/10.0/pcbnew/drc/drc_rule_parser.cpp
15. https://github.com/freerouting/freerouting
16. https://github.com/freerouting/freerouting/blob/master/scripts/benchmark/results/benchmarks.md ; https://github.com/freerouting/freerouting/discussions/508
17. https://gitlab.com/kicad/code/kicad/-/tree/10.0/demos
18. https://github.com/DAC-2020-Submission-1703/PCB-Benchmarks
19. https://github.com/PCBench/PCBench
20. https://arxiv.org/abs/2607.05915
21. https://github.com/LGAI-Research/PCBWorld
22. https://github.com/digailab/PCB-Bench
23. https://arxiv.org/abs/2607.03261 ; https://arxiv.org/abs/2608.04434
24. https://github.com/LukeVassallo/RL_PCB
25. https://github.com/tangsanli5201/DeepPCB
