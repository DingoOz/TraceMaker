# 08 — KiCad integration

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).
> Facts below were verified on 2026-10-02; sources in [`../research/kicad-and-benchmarks-2026-10.md`](../research/kicad-and-benchmarks-2026-10.md).

## 1. Target versions

| Item | Target |
|---|---|
| KiCad | **10.0.x** (10.0.6 stable as of 2026-08-29); read 9.0 files too; track KiCad 11 (master) changes |
| Board file version | `20260206` (10.0). Accept older versions KiCad 10 accepts; **write the same version that was read** |
| Schematic file version | `20260306` (10.0) |
| IPC client | `kicad-python` (`kipy`) 0.8.x |
| `kicad-cli` | 10.0.x, natively or in Docker `kicad/kicad:10.0.6` (no GUI needed) |

## 2. Three integration paths

| Path | Used for | How |
|---|---|---|
| **Files** (primary) | CLI, batch, benchmarks, schematic-to-board | Native C++ s-expression readers/writers for `.kicad_pcb`, `.kicad_sch`, `.kicad_pro` (JSON), `.kicad_dru`, `.kicad_mod`, `fp-lib-table`/`sym-lib-table` |
| **`kicad-cli`** (judge and cross-check) | DRC sign-off, netlist cross-check, zone refill | Subprocess; `pcb drc --format json --severity-all --exit-code-violations --all-track-errors [--schematic-parity] [--refill-zones]` |
| **IPC API** (live in pcbnew) | Plugin: route the open board, show results immediately, one undo step | Python plugin with `kipy`; out-of-process engine |

## 3. File I/O (`src/io/kicad`)

- One generic **s-expression lexer/parser** producing a lossless tree (tokens, original formatting of
  untouched nodes, unknown nodes preserved). Typed views (board, footprint, pad, track…) read from the
  tree; writes patch the tree.
- **Round-trip guarantee**: parse → write without changes is byte-identical (gate of M1). Changes touch
  only the nodes TraceMaker owns: footprint `at`/`layer`/flipped pads, new `segment`/`arc`/`via` nodes,
  removed tracks it ripped.
- **Project rules**: `.kicad_pro` (JSON) holds net classes, net-class patterns/assignments, board design
  settings (min track width, clearances, via sizes, hole clearances), and severity settings. `.kicad_dru`
  holds custom rules (constraint keywords in the research note §A). The rule compiler (doc 03) consumes both.
- **Schematic**: netlist from the hierarchical `.kicad_sch` set (symbols, pins, labels, power symbols, net
  class directives, DNP / exclude-from-board flags). Cross-checked in tests against
  `kicad-cli sch export netlist --format kicadsexpr`.
- **Footprints**: from the board if present; otherwise resolved through `fp-lib-table` to `.kicad_mod` files
  (schematic-to-board mode builds the footprints itself, like "Update PCB from Schematic").
- **Specctra DSN reader / SES writer** for the Freerouting fixture set, plus a **DSN writer** so Freerouting
  can be run as a baseline on our own `.kicad_pcb` fixtures (KiCad 10 has no `kicad-cli` DSN export, and the
  SWIG exporter disappears in KiCad 11).

## 4. Rule coverage policy

- Every rule TraceMaker cannot interpret is reported as a warning with its location, and the router uses
  the most conservative interpretation (for example, the largest clearance that could apply).
- Phase 1 custom-rule conditions: `A.NetClass`, `A.NetName`, `A.Type`, `A.Layer`, `A.insideArea(...)`,
  `A.intersectsArea(...)`, `A.isPlated()`, `A.Pad_Type`, `A.Reference`, boolean `&& || !`. Others later.
- The router also enforces `disallow track/via` by net and layer, and `physical_hole_clearance` between holes
  and copper of any net (doc 05 §27). Positional `disallow` rules are warned and left to the DRC.

## 5. IPC plugin (`kicad_plugin/`)

Facts that shape it:
- KiCad 10 IPC runs only with a **running pcbnew GUI** (headless server is a KiCad 11 feature).
- 10.0 has **no IPC call to read design rules** and **no IPC call to run DRC**. The plugin therefore reads the
  rules from the project files on disk (the plugin knows the project path) and runs `kicad-cli pcb drc` on a
  saved copy when the user asks for sign-off. On KiCad 11 it switches to `GetBoardDesignRules` /
  `GetCustomDesignRules`.

Flow:
1. Plugin (`plugin.json`, Python, own venv with `kicad-python`) starts or connects to `tracemakerd`.
2. Reads the board via `SaveDocumentToString` (exact file content) — simplest lossless transfer — plus
   project files from disk.
3. Engine routes (and places, if the user chose a placement mode); the viewer URL opens in a browser for the
   live view.
4. Results are applied in **one commit** (`begin_commit` → `CreateItems`/`UpdateItems`/`DeleteItems`/
   `FlipItems` → `push_commit`), so a single Ctrl-Z undoes the whole run. `RefillZones` afterwards.
5. Packaged for KiCad's Plugin and Content Manager (PCM).

```python
# sketch (kipy 0.8; exact class names to be checked against the installed version during M11)
from kipy import KiCad
kicad = KiCad()                               # uses KICAD_API_SOCKET / KICAD_API_TOKEN
board = kicad.get_board()
text = board.save_to_string()                 # SaveDocumentToString
result = engine.route(text, project_dir, mode="eco")
commit = board.begin_commit()
board.update_items(result.moved_footprints)   # position/orientation
board.create_items(result.new_tracks + result.new_vias)
board.remove_items(result.ripped_items)
board.push_commit(commit, "TraceMaker route")
board.refill_zones()
```

## 6. DRC judge (bench and sign-off)

```
kicad-cli pcb drc --format json --severity-all --all-track-errors --exit-code-violations \
                  --refill-zones --output out.json routed.kicad_pcb
```

- Run on the **input** board and on the **output** board; the score is the multiset difference by
  violation type and location ("added errors"), so pre-existing problems are not blamed on the router.
- Unconnected items are counted separately (they measure completion, not legality).
- Exit code 5 means violations exist; parse the JSON for the details.

## 7. Pitfalls to design around

- Footprint flips mirror pad layers and change rotation conventions; test flips against KiCad's output.
- KiCad rotation is counter-clockwise in a y-down coordinate system (see `pcbgolf/placer.py` `rot_pt`).
- Zones must be refilled after routing; never write stale fills (use `--refill-zones` for the judge).
- Net-class assignment can come from the schematic (directives), pattern rules in `.kicad_pro`, or the
  board; resolve in KiCad's priority order.
- Locked items (`locked` flag) and user groups must never move or be ripped.

## 8. Implementation status (M11)

- **IPC action plugin** (`kicad_plugin/`): saves a copy of the open board with its project (`save_as(...,
  include_project=True)`; the rules come from the copied project files), routes it, and adds the new tracks and
  vias with `begin_commit` → `create_items` → `push_commit` (one undo step). Since 2026-10-06 also, each by an
  environment variable (README): `TRACEMAKER_REROUTE=1` removes the unlocked tracks and vias and routes the board
  again (`route --reroute`; locked copper, arcs and zones stay); `TRACEMAKER_PLACE=<mode>` runs `tracemaker-place`
  first and moves the footprints with `update_items` (side changes are not applied; when parts move, the old
  copper is routed again and the zones are refilled); `TRACEMAKER_REFILL=1` refills the zones. Removal, moves and
  new copper are one commit. KiCad records a zone refill as a step of its own, so with a refill the result takes
  two undos; that is why refill is off unless footprints moved. Not built: a long-running `tracemakerd` (each run
  starts the engine afresh). The plugin is tested offline against kicad-python 0.8 (`test_offline.py`).
- **Round trip in a running KiCad GUI** (`scripts/kicad_gui_roundtrip.py`, KiCad 10.0.6): pcbnew from the Docker
  image runs on an Xvfb display with the API server enabled (`kicad_common.json`), the first-run wizard is
  clicked away through XTEST, and the plugin talks to the live board over the API socket. On
  `multichannel_mixer-unrouted`: tracks 140 → 316 and vias 19 → 21 in the open board (80 connections in 30 s); one
  Ctrl+Z gives back 140 / 19, Ctrl+Y the result; KiCad's DRC of the board saved from the GUI: unconnected 148 → 65,
  no new errors. With `--place refine`: 65 footprints moved, old copper routed again, zones refilled, two undos
  restore tracks, vias and footprint positions, no new DRC errors. The script is not a ctest (it needs Docker,
  Xvfb and about three minutes); screenshots in `report/m11-gui/`.
- **Engine access**: the plugin imports the Python module `tracemaker` when it can and otherwise runs the
  `tracemaker` binary (`route --emit-items`). `TRACEMAKER_ARGS` uses CLI spelling for both (decision D17).
- **Python bindings** (`bindings/`, pybind11, CMake option `TM_BUILD_PYTHON`, on when pybind11 is found and the
  build is not a sanitizer build): `read_board`, `route`, `emit_items`, `drc`. `route` calls
  `tmk::app::run_route_job` (`src/app/route_job.hpp`), the function behind the CLI's `route` subcommand, so both
  give byte-identical boards (ctest `python_bindings` checks this on a PCBench board). The GIL is released while
  routing and checking. The module is built into `build/<preset>/python/` (D18).
- **PCM package**: `scripts/make_pcm_package.py` writes `build/pcm/tracemaker-<version>.zip` (metadata schema v2
  as shipped with KiCad 10, `type: plugin`, `runtime: ipc`, `kicad_version: 10.0`, the plugin under `plugins/`, a
  generated 64×64 `resources/icon.png`; reproducible archive). `scripts/validate_pcm.py` checks the layout and
  metadata and also runs kicad-python's official validator when it is installed; ctest `pcm_package` builds and
  validates the archive. The engine is not bundled by default (D19); `--binary`/`--module` bundle the Linux builds
  for use on the same machine. Installed through the Plugin and Content Manager of a running KiCad 10.0.6 (headless, as above; Install from
  File…): the manager lists TraceMaker 0.1.0 as installed and compatible, and the files are in
  `3rdparty/plugins/org_tracemaker_autoroute/` (`report/m11-gui/pcm-installed.png`). Not checked: that KiCad then
  builds the plugin's virtual environment and shows the toolbar button (the container had no network for pip).
