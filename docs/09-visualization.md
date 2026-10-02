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
 engine threads ─▶ per-thread SPSC rings ─▶ publisher thread ─┬─▶ replay log (FlatBuffers + zstd, keyframes)
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
  the viewer opens a log file through the engine's `replay` mode and can seek anywhere.
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
| Schema | FlatBuffers, shared `.fbs` compiled for C++ and TypeScript |

## 7. Offline outputs

- `tracemaker render` writes PNG/SVG snapshots and an MP4 time-lapse of a replay (headless WebGPU or a
  CPU raster path) for reports and the benchmark dashboard.
