# 13 — Viewer protocol (v1, JSON over WebSocket)

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). v1 uses JSON for speed of development;
> FlatBuffers (doc 09) replaces it once the message set is stable. Coordinates are integer nanometres.

The engine serves the viewer at `http://<host>:<port>/` and streams messages on `ws://<host>:<port>/ws`.
On connect the server sends the latest `board` snapshot, then every later message in order. Messages marked
*transient* may be dropped under load; all others are state and must be delivered.

| type | fields | meaning |
|---|---|---|
| `board` | `name`, `bbox:[x0,y0,x1,y1]`, `layers:[{name,index}]` (copper, top first), `nets:[name]`, `outline:[[[x,y],…],…]`, `pads:[pad]`, `tracks:[track]`, `vias:[via]`, `zones:[{layer,net,polys:[[[x,y],…]]}]`, `footprints:[{ref,x,y,angle,back,bbox}]` | Full scene (sent on connect and after big changes) |
| `track_add` | `track` | A track was committed |
| `track_remove` | `id` | A track was removed (rip-up) |
| `via_add` / `via_remove` | `via` / `id` | Same for vias |
| `footprint_move` | `ref`, `x`, `y`, `angle` | Placement change (pads follow; resend `board` if simpler) |
| `ratsnest` | `edges:[[ax,ay,bx,by,net],…]` | Current unrouted connections |
| `frontier` *(transient)* | `conn`, `layer`, `pts:[[x,y],…]` | Sampled points of an active search frontier |
| `path_try` *(transient)* | `conn`, `pts:[[x,y,layer],…]` | A candidate path being evaluated |
| `failure` | `conn`, `net`, `rung`, `cause`, `a:[x,y]`, `b:[x,y]`, `blockers:[track/via/pad ids]`, `region:[x0,y0,x1,y1]` | A failed attempt (failure memory) |
| `escape_plan` | `corridors:[{id, net, pad:"REF.NUM", via, pts:[[x,y],…]}]` | Escape corridors reserved for dense-package pins (M9); `id` is the board pad index. Sent at the start and after each restart; the viewer draws them (toggle X) |
| `escape_release` | `id` | The pin is connected (or given up): its corridor is gone |
| `escape_dead` | `id`, `net`, `pad`, `p:[x,y]`, `why` | A pin enclosed by fixed copper even for a negotiated, necked-down search: not retried (red cross in the viewer) |
| `heatmap` | `name`, `x0`, `y0`, `cell`, `w`, `h`, `layer`, `max`, `data:[…]` (row-major, 0–255) | Congestion / history / learned-penalty overlays |
| `stats` | `stage`, `iteration`, `routed`, `total`, `unrouted`, `rips`, `failures`, `elapsed_s`, `extra:{}` | Progress counters (≤ 10 Hz) |
| `stage` | `name`, `state:"begin"|"end"`, `detail` | Pipeline stage changes |
| `log` | `level`, `text` | Human-readable log line |

Object shapes:
- `pad`: `{id, net, layers:[copper index…], shape:"circle"|"segment"|"polygon", pts:[[x,y],…], r}`
  (same rounded-core model as the DRC: circle = 1 point + r; oval = 2-point segment + r; polygon may have r > 0)
- `track`: `{id, a:[x,y], b:[x,y], w, layer, net}`
- `via`: `{id, p:[x,y], d, drill, net, top, bottom}`

Ids are stable for the lifetime of a session (the engine assigns them).
