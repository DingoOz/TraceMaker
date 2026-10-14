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
- **Legacy zone fills**: KiCad 5 `filled_polygon` nodes without a layer inherit the zone's copper
  layer only when its copper mask contains exactly one bit. Explicit fill layers take precedence
  (even an unknown explicit layer); missing, unknown or multi-copper-layer zones never guess a fill
  layer. This affects the typed copper/connectivity view only; the original text stays untouched (D105).
- **Project rules**: `.kicad_pro` (JSON) holds net classes, net-class patterns/assignments, board design
  settings (min track width, clearances, via sizes, hole clearances), and severity settings. `.kicad_dru`
  holds custom rules (constraint keywords in the research note §A). The rule compiler (doc 03) consumes both.
  The reader seeds supported KiCad 10 defaults and applies legacy board rules. Project values overlay
  defaults only where KiCad's legacy migration permits loading them; missing keys preserve current
  values and explicit zeros remain zero (D108).
- **Schematic**: netlist from the hierarchical `.kicad_sch` set (symbols, pins, labels, power symbols, net
  class directives, DNP / exclude-from-board flags). Cross-checked in tests against
  `kicad-cli sch export netlist --format kicadsexpr`.
- **Footprints**: from the board if present; otherwise resolved through `fp-lib-table` to `.kicad_mod` files
  (schematic-to-board mode builds the footprints itself, like "Update PCB from Schematic").
- **Specctra DSN reader / SES writer** for the Freerouting fixture set, plus a **DSN writer** so Freerouting
  can be run as a baseline on our own `.kicad_pcb` fixtures (KiCad 10 has no `kicad-cli` DSN export, and the
  SWIG exporter disappears in KiCad 11).

### 3.1 Supported native rule defaults (D108)

Defaults are taken from KiCad 10.0.3's
[`include/board_design_settings.h`](https://github.com/KiCad/kicad-source-mirror/blob/10.0.3/include/board_design_settings.h),
[`BOARD_DESIGN_SETTINGS` constructor](https://github.com/KiCad/kicad-source-mirror/blob/10.0.3/pcbnew/board_design_settings.cpp)
and [`common/netclass.cpp`](https://github.com/KiCad/kicad-source-mirror/blob/10.0.3/common/netclass.cpp).
The constructor sets `m_resetParamsIfMissing = false`: absent project keys keep constructor values or
values read from the legacy board. In particular, use the constructor's 0.1 mm annular width, not the
different reset default in the JSON parameter declaration.

| Supported board constraint | Default (mm) |
|---|---:|
| Overall clearance / minimum connection width | 0 / 0 |
| Minimum track width | 0.2 |
| Minimum via diameter / annular width | 0.5 / 0.1 |
| Minimum through-hole drill | 0.3 |
| Minimum microvia diameter / drill | 0.2 / 0.1 |
| Hole-to-copper / hole-to-hole clearance | 0.25 / 0.25 |
| Copper-to-edge clearance | 0.5 |
| Silk clearance / mask-to-copper clearance / mask web width | 0 / 0 / 0 |

The native Default class has clearance/track width 0.2/0.2 mm, via diameter/drill 0.6/0.3 mm,
microvia diameter/drill 0.3/0.1 mm and differential-pair width/gap/via-gap 0.2/0.25/0.25 mm.
Class dimensions are routing preferences, not permission to violate a board or custom minimum.
Project classes inherit missing/null fields from Default. Native legacy net-class migration takes
precedence over project class definitions and assignments. Board-local fields retain their existing precedence.
Missing or malformed projects remain warned about; they do not turn supported minimums into zero.

**Legacy migration precedence:** the reader follows KiCad 10.0.6's
[`BOARD::SetProject`](https://github.com/KiCad/kicad-source-mirror/blob/10.0.6/pcbnew/board.cpp)
and [`parseSetup` / `parseNETCLASS`](https://github.com/KiCad/kicad-source-mirror/blob/10.0.6/pcbnew/pcb_io/kicad_sexpr/pcb_io_kicad_sexpr_parser.cpp):
tokens that set `m_LegacyDesignSettingsLoaded` (for example `trace_min`, `via_min_size`,
`user_trace_width` or `defaults`) suppress loading the project's board settings as a whole,
including minimums and severities. The reader detects those exact legacy tokens, not merely `setup`.
Local `pad_to_mask_clearance`, `solder_mask_min_width`, origins, stackup and plot settings do not
set that flag and do not suppress project rules. Present project keys, including zero, remain
authoritative on boards without a legacy design-settings trigger.

Net classes migrate independently: any root `net_class` transfers legacy classes and assignments
over project classes/patterns, but does not itself suppress project board minimums. Conversely,
legacy design-setting tokens alone do not suppress project net classes.

`tests/integration/no_project_defaults.py` generates isolated native DRC probes for absent, empty,
partial and zero-valued projects, project overrides and legacy preservation/migration, then routes
a legacy 0.15 mm class under the built-in 0.2 mm minimum and checks preservation of a wider legacy
class despite a conflicting project class. Assertions use violation types and actual written
copper widths, not rendered diagnostics or echoed options. Unit tests cover default boundaries,
cross-property legacy suppression, nontriggering local fields and independent class migration.

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

## 9. Simple Route JSON (`src/io/srj`, D90)

tscircuit publishes autorouting problems as Simple Route JSON (SRJ): one JSON object with the layer count, the
board bounds, rectangular or round obstacles, the connections to make and a few scalar rules; a solution adds
`traces`. TraceMaker reads and writes it at the edge only. The model, the exact check and the judge stay KiCad's:
a problem becomes a KiCad board with its project, the usual pipeline routes and judges it, and the routed board
is written back as traces.

```
tracemaker srj-import problem.json -o board.kicad_pcb [--clearance-mm X] [--hole-to-hole-mm X] [--json summary.json]
tracemaker route board.kicad_pcb -o routed.kicad_pcb ...
tracemaker srj-export routed.kicad_pcb --problem problem.json -o solved.json [--json summary.json]
```

Field meanings are taken from `lib/types/srj-types.ts` and the code around it in
github.com/tscircuit/tscircuit-autorouter (MIT, read at `5bcf774`) and from the page `advanced/simple-route-json`
on docs.tscircuit.com, which is older and says less.

**Geometry.** SRJ is millimetres with y up; KiCad has y down. A point (x, y) becomes (x, −y) and back, with no
offset, so the board looks the same on screen and `ccwRotationDegrees` is KiCad's pad angle unchanged. Layers
are `top`, `inner1` … `innerN`, `bottom` in stack order and map to `F.Cu`, `In1.Cu` …, `B.Cu`. A problem with one
layer gets a `B.Cu` that a rule area closes to tracks and vias, since a KiCad board has two copper layers at
least. The board edge is the `outline` polygon if there is one, else the `bounds` rectangle.

**Nets.** SRJ has no net table. An obstacle lists ids in `connectedTo` (connection names, port ids, net ids of the
tool that made the file) and a connection point carries a `pointId` and a `pcb_port_id`. The reader forms the same
groups as tscircuit's `getConnectivityMapFromSimpleRouteJson`: ids that appear together are one group, and an
obstacle and a point with the same centre (to 0.01 mm) and layers are too. A group that holds a connection is a
net, named after its first connection. An obstacle in a group without a connection gets no net: it is copper to
keep clear of, and KiCad's ratsnest must not ask for a connection the problem does not ask for. An obstacle with
an empty `connectedTo` is a keep-out and joins nothing.

**Obstacles.** One footprint per `componentId` at the origin; every obstacle is a pad at its absolute position.

| Obstacle | KiCad |
|---|---|
| one layer | SMD pad, `rect` (rotated by `ccwRotationDegrees`) or `circle` (`shape: "circle"`, diameter = `width`) |
| several layers, in a net | plated through-hole pad. SRJ has no drill size: the drill is half the smaller side, at least 0.1 mm, as tscircuit's own conversion to Circuit JSON does. A through-hole pad spans the whole stack even if the obstacle lists fewer layers (warned) |
| several layers, no net | one SMD pad per layer: nothing has to join the layers, so no hole is invented |
| `isNonPlatedHole` | non-plated hole; a rectangular one is a slot plus a rule area over the rectangle, since the slot leaves the corners out |
| `isCopperPour` | zone of its net without a fill: tscircuit's pipelines leave pours out of their obstacle sets, so a pour is copper to be poured again around the routing (refill in KiCad) |

A connection point that lies on no obstacle of its net gets a round pad of the track width, because KiCad's
connectivity needs a pad to end on. The converter counts the point-to-point connections the problem asks for and
the ratsnest the board implies (pads − 1 per net) and warns when they differ.

**Rules.** `<board>.kicad_pro` carries one net class and the board minimums, and nothing the problem does not
state:

| SRJ | KiCad |
|---|---|
| `minTraceWidth` | minimum track width |
| `nominalTraceWidth` (else `minTraceWidth`) | track width of the Default class; a connection's own `nominalTraceWidth` puts its net in a class of that width |
| `min_via_pad_diameter` / `minViaPadDiameter` / `minViaDiameter`, `min_via_hole_diameter` / `minViaHoleDiameter` | via size and drill of the class, minimum via diameter, drill and annular width; resolved as tscircuit's `getViaDimensions` (pad 0.3 mm and hole half the pad when absent) |
| `defaultObstacleMargin` | copper clearance (class and board minimum). Absent: 0.15 mm, the fallback tscircuit's routers use, with a warning; `--clearance-mm` sets it |
| `minTraceToPadEdgeClearance`, `minViaEdgeToPadEdgeClearance` | custom rules in `<board>.kicad_dru` (track or via against pad) when they differ from the clearance |
| `minTraceToHoleEdgeClearance` | minimum hole clearance (KiCad applies it to plated holes too) |
| `minViaHoleEdgeToViaHoleEdgeClearance`, `minPlatedHoleDrillEdgeToDrillEdgeClearance` | minimum hole-to-hole distance (the larger); `--hole-to-hole-mm` sets it |
| `minBoardEdgeClearance` | minimum copper-to-edge clearance |
| `allowBlindAndBuriedVias` | the board setting; `route --blind-vias` then uses them |
| `allowViaInPad` | no KiCad setting: reported, and `route --via-in-pad` is the user's choice |
| `minPadEdgeToPadEdgeClearance` | not written: it concerns fixed pads only |

Rules the problem leaves out are zero (no rule), not KiCad's defaults. Preloaded `traces`, `differentialPairs`,
`buses`, jumpers, off-board connections and `terminalVia` hints are read and reported as not imported.

**Determinism.** The output depends only on the problem and the options: obstacles, footprints and nets keep the
file's order, numbers are exact nanometres, and UUIDs are counted from a fixed Philox stream, not drawn. Two
conversions give the same bytes (tested).

**Export.** `srj-export` writes the problem unchanged with `traces` set. Nets are matched by name and pads by
position. A routed net is a graph (track pieces, vias, pads), joined wherever copper of the net touches, as KiCad 10's
connectivity joins it: a track end or a via on another track, two tracks that cross, a track or a via over a pad; each
connection gets the shortest path through its net's copper between the pads of its points, as wire points and
`via` points (with `via_diameter`, `via_hole_diameter`, and the spanned `layers` when the problem allows blind
vias). The trace starts and ends at the problem's own coordinates. A connection without a path is listed as
unrouted and the exit code is 3. Copper that no connection path used (a stub, a partial route) follows as traces
named `<net>_extra_<n>`, so a checker reading the file sees all the copper. Limits: arcs are exported as
polylines; a path that changes layer through a through-hole pad has no `via` point there.

Tests: `tests/test_srj.cpp` on the hand-made `tests/srj/mixed.json` (rotated rectangle, circle, plated hole with a
multi-layer point, keep-out, non-plated hole, pour, outline, merged connections, a point on bare board) and on
inline problems (one layer, a slot, tracks that only touch);
`tests/integration/srj_roundtrip.py` imports `tests/srj/tiny.json`, routes it, exports it and checks every trace
point against the routed copper and, when `kicad-cli` is there, KiCad's ratsnest and DRC. The benchmark built on
this is doc 10 §7.

### 9.1 Fan-out problems (M14, D96)

`tracemaker srj-import` handles tscircuit's fan-out benchmark (`fanout31`) with these additions: plane terminations
(`buses[].termination` of type `plane`) become zones of the net on that layer covering the bounds; a breakout point
(`pointId` starting `pcb_breakout_point`) takes the layer its bus gives in `connectionExitTargets`; `--unpointed-keepout` turns the
pads of a net that no connection point lies on into keep-outs; `--outline-margin-mm` grows the outline (breakout points lie on the
bounds); the extension field `tracemakerTrackLayers` (layer names) closes the other layers to tracks with a rule area that lets vias
through. `bench/prepare_fanout31.py` sets them for the 72 cases; `bench/fanout31_run.py` routes and scores them.
