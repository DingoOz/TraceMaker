# 09 — Real-time visualisation

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md).

## 1. Goals

- Watch placement and routing **live**, smoothly, on boards with up to millions of primitives.
- Make the algorithms legible: what the search is exploring, why something failed, what was learned.
- Look outstanding: a dark, glowing, layered PCB aesthetic that stays readable.
- Work locally and remotely (the engine runs on a headless server).
- Scrub back and forth through the whole run afterwards (replay).

## 2. Architecture

```
 engine threads ─▶ per-thread SPSC rings ─▶ publisher thread ─┬─▶ replay log (JSON lines in zstd frames, keyframe; D45)
                                                               └─▶ WebSocket server ─▶ browser viewer
                                                                     (snapshot on connect, then deltas,
                                                                      coalesced to ≤ 60 Hz, LOD-filtered)
```

- **Protocol**: binary WebSocket frames carrying FlatBuffers messages (zero-copy decode in the browser).
  On connect: a full scene snapshot (board, layers, footprints, current copper). Then deltas: added/removed
  objects by id, overlay updates (heatmaps as compressed tiles), transient events (frontiers, ghosts).
- **Back-pressure**: the publisher keeps the latest value per overlay and merges deltas while the socket is
  busy; transient visual events are droppable, state deltas are not.
- **Replay**: the same message stream is written to disk with a keyframe (full snapshot) every N seconds;
  the viewer opens a log file through the engine's `replay` mode and can seek anywhere. Built so far (D45): the
  JSON messages, one per line, in independent zstd frames with the snapshot as its own frame; replay plays from
  the start (seeking needs the control channel and periodic keyframes, not built yet).
- **Control channel** (viewer → engine): pause/resume/step, change speed, pick an object, request detail
  (e.g., a failure record), adjust visual LOD. Changes to the routing itself stay in the CLI/plugin.
- **Security**: binds to `127.0.0.1` by default; remote access through an SSH tunnel, or `--listen 0.0.0.0`
  with a random token in the URL and optional TLS.

## 3. Renderer (browser, WebGL2 baseline, WebGPU when available)

Browser status (Oct 2026): WebGPU is on by default in Chrome/Edge on Windows/macOS/ChromeOS, depends on
drivers in Chrome on Linux (≥ 144), is **still behind a flag in Firefox on Linux**, and ships in Safari 26.
The server is Linux and users may view from Linux, so **WebGL2 is the baseline renderer** and WebGPU an
automatic upgrade (compute-shader LOD and bloom are faster there). All shaders are written once against a
thin backend interface; the WebGL2 path uses fragment-shader SDFs and render-to-texture for compute work.


- **Instanced primitives**: one draw per layer per primitive type. Tracks are instanced quads with
  round caps computed in the fragment shader (signed distance to the segment, KiCad GAL-style), arcs
  likewise; pads/vias are SDF circles, rounded rectangles and polygons (pre-triangulated in a worker).
- **Signed-distance anti-aliasing** at every zoom; copper looks crisp from board view to 10 µm detail.
- **Layer compositing**: each copper layer renders to its own target; a compositor blends layers with
  per-layer colour and opacity, the active layer on top, others dimmed (KiCad-like "high contrast" mode).
- **Bloom/glow pass** (downsample–blur–upsample) applied only to *active* items: the trace being routed,
  the search frontier, the moving footprint. Static copper stays matte so glow carries meaning.
- **Heatmap overlays** as float textures (congestion, history, learned penalties, RUDY, density) with
  perceptually uniform palettes (viridis/inferno/turbo selectable), blended under or over the copper.
- **Level of detail**: below a zoom threshold, tracks become a density texture computed on the GPU
  (compute shader), so pan/zoom stays at 60 fps on very large boards.
- **Picking**: an id buffer pass; hover shows net, class, rule, routing history of the object.

## 4. What the user sees

| View element | Content |
|---|---|
| Main canvas | Board, copper per layer, pads, vias, zones, ratsnest (airwires fade as connections complete) |
| **Search frontier** | The current A* frontier as a fading, glowing wave per active thread (sampled, not every node) |
| **Rip-up** | Ripped tracks dissolve into particles; rerouted ones draw in along their path |
| **Failure ghosts** | Failed attempts shown as dashed red paths for a few seconds, blockers outlined in red, the over-full cut drawn as a red bar with "demand / capacity" |
| **Placement motion** | Footprints glide (interpolated between frames) during global placement; the electrostatic density appears as a heat field; ECO moves pulse the moved part |
| **Overlays** | Congestion, history `h`, learned penalties, RUDY, pin density, escape plan |
| Layer stack | Toggle/solo layers; an exploded 2.5-D tilted view of the stack (each layer a translucent plane, vias as vertical glowing pillars) |
| Timeline | Score, unrouted count, DRC errors, overflow over time; stage bands; scrubber for replay |
| Learning panel | Bandit arm probabilities per stage, nogood count, top conflict-graph edges, per-net activity |
| Stats HUD | Stage, iteration, connections/s, GPU/CPU utilisation, work units, ETA |
| Minimap | Whole board with the viewport and hot spots |
| Inspector | Click a net/connection to see its escalation ladder history and failure records |

## 5. Visual style

- Dark background (near-black navy), KiCad-familiar layer colours by default (F.Cu red, B.Cu blue, inner
  layers distinct hues) with an alternative "neon" palette; colour-blind safe option.
- Motion is purposeful: 150–300 ms easing for state changes; transient effects fade out.
- Typography: one monospace font for numbers, one sans for labels.

## 6. Technology

| Part | Choice |
|---|---|
| Viewer | TypeScript, Vite, own renderer: WebGL2 baseline (GLSL ES 3.0) + WebGPU (WGSL) backend. deck.gl's WebGPU path is still "experimental" and PixiJS prefers WebGL, so neither is the core; PixiJS v8 is acceptable for UI overlays |
| Debug viewer | **Rerun** (MIT/Apache-2.0, C++ SDK) for kernel and algorithm debugging during M4–M6, before the product viewer has all panels; not a product dependency ("expect breaking changes") |
| UI chrome | Lightweight (Preact or Svelte) panels over the canvas; charts with a small canvas plotting library |
| Engine server | uWebSockets (Apache-2.0) or Boost.Beast; serves the built static viewer too |
| Schema | FlatBuffers, shared `.fbs` compiled for C++ and TypeScript, once the message set is stable; until then JSON (doc 13), compressed with zstd in recordings (D45) |

## 7. Offline outputs

- `tracemaker render` writes PNG/SVG snapshots and an MP4 time-lapse of a replay (headless WebGPU or a
  CPU raster path) for reports and the benchmark dashboard.

## 8. Implementation status (v1)

v1 implements the JSON protocol of [`13-viewer-protocol.md`](13-viewer-protocol.md) end to end, recording and
replay (plain or zstd-compressed logs) and heatmap overlays. FlatBuffers (deferred, D45), seeking in a replay,
the control channel, the minimap and the WebGPU backend are not built yet.

### Heatmaps and replay log

- The router keeps two coarse grids (`route/heat_grid.hpp`, at most 128 cells on the long side, whole-µm cells
  over the routing lattice) while a sink is attached: **`expansions`**, A* expansions per cell (one sample per
  64 expansions, cumulative over the run), and **`history`**, the largest PathFinder history cost per cell. Both
  are sent as `heatmap` messages (layer -1, square-root scaled bytes, `"scale":"sqrt"`) every
  max(200k, budget / 40) expansions (2M without a work budget) and once at the end. They are counted by work, not
  wall time, so a recording is the same for the same input and seed; the router never reads them, so routing is
  unchanged (`record_replay` compares boards routed plainly, recorded and viewed byte for byte).
- The server keeps the latest grid per name, so a viewer that connects late (or a replay) sees the current
  overlay. The viewer draws the selected one over the copper as an R8 texture with linear filtering and the
  inferno palette (zero cells transparent); `M` or the Overlay chips choose it, `#heat=<name>` in the URL
  preselects it, and the legend shows the raw maximum.
- `tracemaker route --record FILE` writes one JSON message per line with a leading `"t"` (seconds). If FILE
  ends in `.zst` the log is a sequence of independent zstd frames (`server/replay_log.hpp`): the board snapshot
  is a frame of its own and later frames close every 1 MiB of text, so a killed run loses at most the open
  frame and any zstd tool reads the log (`zstd -dc run.jsonl.zst | jq`). On C-BISCUIT and CANadapter the
  compressed log is 12–13× smaller than the plain one (≈ 20 kB vs 260 kB). `tracemaker-view --replay` detects
  the format from the content.

### Engine side (`src/server/`)

| Target | Contents |
|---|---|
| `tm_viz_messages` (`tm::viz`) | `server/messages.hpp`: `board_snapshot_json(board, name)` plus builders for `track_add/remove`, `via_add/remove`, `footprint_move`, `ratsnest`, `frontier`, `path_try`, `failure`, `stats`, `stage`, `log`, `heatmap`, and `message_type()`. Depends only on model, drc and nlohmann_json, so the router can link it without Boost |
| `tm_server` (`tm::server`) | `tmk::server::ViewerServer : events::Sink`: Boost.Beast HTTP + WebSocket on one background I/O thread. Serves static files (MIME types, percent-decoding, `..` and symlink escape rejected) from `<repo>/viewer/dist` or `ServerOptions::web_root`; WebSocket at `/ws`. Default bind `0.0.0.0:8766` (8765 is the dev progress site) |
| `tracemaker-view` | `tracemaker-view <board.kicad_pcb> [--port N] [--host H] [--web DIR] [--demo] [--keep-tracks] [--replay LOG [--speed S] [--loop]]` |
| `tm_replay` (`tm::replay`) | `server/replay_log.hpp`: `ReplayWriter` (plain, or zstd frames for `.zst`) and `ReplayReader` (format detected from the magic number; a truncated tail is ignored, corrupt frames throw). Links libzstd only |
| `tm_server_tests` | Catch2: message builders, snapshot of `pic_programmer` (skipped if the fixture is missing), HTTP serving and traversal rejection, snapshot-then-deltas delivery, late-client catch-up, back-pressure; replay log round-trips (plain, zstd, multi-frame, truncated, corrupt), `HeatGrid` sizing/scaling, and routing C-BISCUIT with a capturing sink: heatmaps sent, copper identical to routing without one. Registered with ctest |

Server behaviour:
- `publish()` only appends to a mutex-protected inbox and posts a drain to the I/O thread; it never touches
  sockets. `wants_transient()` is true only while a viewer is connected, so producers can skip frontier work.
- The server keeps the latest `board` snapshot and the state since it (tracks/vias added or removed, footprint
  moves, the latest `ratsnest`, `stats` and heatmap per name/layer, and the last 300 `stage`/`log`/`failure`
  messages). A client that connects mid-run receives the snapshot, then that state, then the live stream.
- Per-client back-pressure: beyond 2,000 queued messages or 8 MB, `frontier`/`path_try` are dropped and queued
  `stats` are coalesced to the latest; a new `board` supersedes queued scene deltas; a client more than 256 MB
  behind on state is disconnected (it reconnects and gets a fresh snapshot).
- Additions to the protocol (all optional, ignored by older readers): pads carry `fp` (footprint index), `num`
  (pad number) and `hole:{pts,r}`; footprints carry `value`; the `board` outline is chained into closed loops
  where the pieces join. Arc tracks are sent as straight segments with negative ids (-1, -2, …), so producers can
  number new tracks from `Board::tracks.size()` upwards. Copper graphics (non-pad copper shapes) are not sent yet.

### Viewer (`viewer/`, TypeScript + Vite, no framework)

- WebGL2 renderer written against the raw API (GLSL ES 3.0, `viewer/src/shaders.ts`): instanced capsules with a
  signed-distance fragment shader (tracks, round/oval pads, polygon-pad edges with r > 0, outline, ratsnest,
  footprint boxes); vias as SDF rings with drill; polygon pads ear-clipped; zones and the board substrate filled
  with an even-odd stencil pass (robust to holes and fractured fills).
- Layer compositing: each copper layer is drawn into a 4× MSAA offscreen target, resolved, and composited with
  its opacity, so overlapping copper on one layer never double-blends. "Active on top" draws the active layer
  last and dims the others; layers can be hidden individually. Through-hole pads, holes and vias draw above.
- Effects use additive blending: new tracks glow and decay (two Gaussian halos, 1.4 s), frontier points are
  fading dots (0.6 s), `path_try` is a fading polyline, failures are pulsing red rings at both ends plus a moving
  red dashed line (4.5 s). Effects are suppressed while catching up after a snapshot or a stall.
- HUD: stage, iteration, routed/total with progress bar, unrouted, rip-ups, failures, elapsed, messages/s, FPS,
  `stats.extra`; layer list (click = active, eye = visibility, keys 1–9); toggles for active-on-top (H), zones (Z),
  ratsnest (R), footprints, effects (E), follow activity (L); event log of `log`/`stage`/`failure`; cursor
  position in board millimetres; footprint reference labels; hover tooltip (net, layer, width/size, pad ref)
  from a CPU uniform-grid picker, with the hovered net highlighted.
- WebSocket client reconnects with exponential backoff (0.4–5 s) and applies all queued messages once per
  animation frame; only the newest transient messages of a frame are kept after a stall.
- Views can be linked as `#view=<x mm>,<y mm>,<px per mm>&layer=<n>&hc=0|1&follow=1`.

### Running it

```
cd viewer && npm install && npm run build            # -> viewer/dist (served by the engine)
cmake --build --preset release --target tracemaker-view
build/release/src/server/tracemaker-view bench/data/kicad/demos/video/video.kicad_pcb --demo
# open http://<host>:8766/ ; --demo clears the board's tracks and "routes" them again with synthetic events
cd viewer && npm run dev                              # hot-reload development, proxies /ws to :8766
```

Verified in headless Chromium (SwiftShader WebGL2) on the `video` (7,932 tracks, 4 layers) and
`RoyalBlue54L-Feather` (8 layers, zone fills) demos. Firefox has not been tested on this machine; the renderer
uses only core WebGL2 (instancing, MSAA renderbuffers, `blitFramebuffer`, stencil).
