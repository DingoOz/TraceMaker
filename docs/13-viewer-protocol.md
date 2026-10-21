---
status: current
code: [src/server, viewer]
---

# 13 — Viewer protocol (v1, JSON over WebSocket)

> Part of the TraceMaker plan. Start at [`../PLAN.md`](../PLAN.md). v1 uses JSON for speed of development;
> FlatBuffers (doc 09) replaces it once the message set is stable. Coordinates are integer nanometres.

The engine serves the viewer at `http://<host>:<port>/` and streams messages on `ws://<host>:<port>/ws`. The host is the local machine unless the user asked otherwise;
a viewer that other machines can reach requires a token (`?token=...` once, then a cookie) and every `/ws` upgrade must come from the server's own origin (docs/09, Security).
On connect the server sends the latest `board` snapshot, then every later message in order. Messages marked
*transient* may be dropped under load; all others are state and must be delivered.

| type | fields | meaning |
|---|---|---|
| `board` | `name`, `bbox:[x0,y0,x1,y1]`, `layers:[{name,index}]` (copper, top first), `nets:[name]`, `outline:[[[x,y],…],…]`, `pads:[pad]`, `tracks:[track]`, `vias:[via]`, `zones:[{layer,net,polys:[[[x,y],…]]}]`, `footprints:[{ref,x,y,angle,back,bbox}]`, optional `freeze` (`false` when the engine was started with `route --no-freeze`: the viewer does not hold the scene on a routing stall) | Full scene (sent on connect and after big changes) |
| `track_add` | `track` | A track was committed |
| `track_remove` | `id` | A track was removed (rip-up) |
| `via_add` / `via_remove` | `via` / `id` | Same for vias |
| `footprint_move` | `ref`, `x`, `y`, `angle` | Placement change (pads follow; resend `board` if simpler) |
| `ratsnest` | `edges:[[ax,ay,bx,by,net],…]` | Current unrouted connections |
| `frontier` *(transient)* | `conn`, `layer`, `pts:[[x,y],…]` | Sampled points of an active search frontier |
| `path_try` *(transient)* | `conn`, `pts:[[x,y,layer],…]` | A candidate path being evaluated |
| `failure` | `conn`, `net`, `rung`, `cause`, `a:[x,y]`, `b:[x,y]`, `blockers:[track/via/pad ids]` (still empty), `blocker_nets:[net ids]` (nets of the connections the rejected route would have ripped), `region:[x0,y0,x1,y1]` (the two pads ±1.5 mm, nm). Optional, from issue #8 (a reader that does not know them ignores them; they are present only when the router measured them): `reason` (`boxed_in`, `no_legal_path`, `exact_check`, `rip_limit`, `rolled_back`, ...), `width_nm`, `clearance_nm`, `min_width_nm` (the attempt's track width, the clearance applied, and the narrowest width the rules allow: the board minimum, else 0.1 mm), `gap_nm` (the widest track that gets through from the closest approach on towards the target with routed copper counted; 0 = not even a hairline), `gap_fixed_nm` (the same with only fixed copper counted), `gap_layer`, `at:[x,y]` (where the way is narrowest, or the closest approach when none gets through), `verdict` (`placement`, `rule`, `contested`, `unknown`), `rule` (where the width and clearance came from, and what would open the way), `rejected:[[x,y,layer],...]` (the path that was tried, corners only, at most 64; left out when the search never left the source pad) | A failed attempt (failure memory) |
| `advice.hint` | `kind`, `text`, `origin` (`file` or `advisor`) | One hint of an advice file in use (doc 17); `stage` events named `advisor` mark a consultation |
| `advice.rejected` | `why` | The advisor failed, was too slow, or its advice was refused; the run goes on unadvised |
| `return_path` | `net`, `name`, `detour_mm`, `max_separation_mm`, `voids`, `splits`, `missing_stitches`, `signal:[{layer, pts:[[x,y],…]}]`, `return:[{layer, pts}]` (the return along the trace: over copper its mirror image, across a void the detour; drawn from the end of the trace back to its start), `callouts:[{kind, p:[x,y], text}]` (`signal_pad`, `ref_pad`, `signal_via`, `return_via`, `missing_stitch`, `separation`, `split`) | One net's reference-return audit (issue #7, doc 05 §42). State, kept per net for late clients; coordinates in nm |
| `footprint_check` | `checked`, `ok`, `mismatch`, `unverified`, `untrusted`, `errors`, `warnings`, `footprints:[{ref, lib_id, status, p:[x,y], datasheet?:{id, brand, part, document, revision?, page?, figure?, url?}, issues:[{severity, kind, pad, text, label, p:[x,y]}]}]` (only footprints with an issue) | The footprint check against datasheet land patterns (issue #5, doc 05 §43). `kind`: `pad_count`, `missing_pad`, `extra_pad`, `position`, `size`, `drill`, `pad_type`, `orientation`, `mirrored`, `pattern`, `no_reference`, `untrusted_source`; `text` is a sentence, `label` a few words for a mark on the board. State (the latest one is kept for late clients); coordinates in nm |
| `critical_nets` | `nets:[{name, tier, why, vias, length_mm, unrouted, void_crossings?, detour_mm?, splits?, warnings:[text]}]`, `totals:{nets, vias, void_crossings, over_via_limit}` | The critical nets after a route with `--critical-*` or `--si-rules` (issue #24, doc 05 §44): tier 1 clocks, crystals and RF, 2 high-speed links, 3 memory and fast buses; vias and length added by the route; where the board has a reference plane, how often the net runs over a void of it. Each net's `return_path` message follows. State (the latest is kept for late clients) |
| `escape_plan` | `corridors:[{id, net, pad:"REF.NUM", via, pts:[[x,y],…]}]` | Escape corridors reserved for dense-package pins (M9); `id` is the board pad index. With `--escape-flow` (doc 05 §14) `pts` continues past the dog-bone via site along the planned channels, on the layer the via changes to. Sent at the start and after each restart; the viewer draws them (toggle X) |
| `escape_release` | `id` | The pin is connected (or given up): its corridor is gone |
| `escape_dead` | `id`, `net`, `pad`, `p:[x,y]`, `why` | A pin enclosed by fixed copper even for a negotiated, necked-down search: not retried (red cross in the viewer) |
| `bus` | `name`, `nets:[net,…]` | A bundle of a bus (doc 05 §38): the nets whose connections between two components are routed lane by lane, in lane order. Sent once per bundle when routing starts |
| `bus_lane` | `bus`, `conn`, `net`, `state:"leader"|"followed"|"left"` | A connection of a bundle was routed: it leads, it has its neighbour lane's layers and vias, or it left the bus |
| `bus_skew` | `bus`, `skew`, `limit` | Length matching of a group is done: longest minus shortest member and the limit, nm |
| `heatmap` | `name`, `x0`, `y0`, `cell`, `w`, `h`, `layer`, `max`, `data:[…]` (row-major, 0–255), optional `scale` (`"sqrt"`: byte = round(255·√(v/max)), non-zero cells ≥ 1; absent: linear) | Overlays, latest per name replaces the previous one. The router sends `expansions` (A* expansions per cell, cumulative) and `history` (largest PathFinder history cost per cell), layer -1 |
| `stats` | `stage`, `iteration`, `routed`, `total`, `unrouted`, `rips`, `failures`, `elapsed_s`, `extra:{}` | Progress counters (≤ 10 Hz) |
| `stage` | `name`, `state:"begin"|"end"`, `detail` | Pipeline stage changes |
| `log` | `level`, `text` | Human-readable log line |

Object shapes:
- `pad`: `{id, net, layers:[copper index…], shape:"circle"|"segment"|"polygon", pts:[[x,y],…], r}`
  (same rounded-core model as the DRC: circle = 1 point + r; oval = 2-point segment + r; polygon may have r > 0)
- `track`: `{id, a:[x,y], b:[x,y], w, layer, net}`
- `via`: `{id, p:[x,y], d, drill, net, top, bottom}`

Ids are stable for the lifetime of a session (the engine assigns them).

## Recordings (replay log)

`tracemaker route --record FILE` writes the same messages, one JSON object per line, each with a leading
`"t"` (seconds since the start of the run; portfolio runs keep only the winning variant's events). The first
line is the `board` snapshot of the unrouted board. Transient messages (`frontier`, `path_try`) are not
recorded; `heatmap` messages are. If FILE ends in `.zst` the lines are stored as a sequence of independent
zstd frames (decision D45): the snapshot is a frame of its own, later frames close every 1 MiB of text, and
frames carry checksums. `zstd -dc FILE` gives the plain form. `tracemaker-view <board> --replay FILE` reads
either form (detected from the zstd magic number) and republishes the messages at their recorded times.

## The `failure` verdict (issue #8, D113)

`gap_nm` and `gap_fixed_nm` are the bottleneck of the best way from the closest approach towards the target, measured with the router's own
obstacle tests (so clearances, rules, pads, holes and edges are the lattice's own): a widest-path search over a probing grid in a corridor of
±2 mm around the straight line, at most 12 mm long, whose node value is the widest track legal there (a binary search on the width) and whose way value
is its smallest node; layer changes need a legal via. The verdict follows from the numbers, `W` being the net class width and `M` the narrowest legal width:

| Verdict | When | Next step |
|---|---|---|
| `contested` | `gap_fixed_nm >= W`: fixed copper alone leaves room for `W`, `gap_nm < W`: routed copper of other nets closes the way | negotiation, not a placement edit |
| `rule` | `M <= gap_fixed_nm < W` (a narrower legal track passes), or the board minimum clearance would pass `M` | change the net class width or the clearance rule |
| `placement` | `gap_fixed_nm < M` and the board minimum clearance does not help | move a part |
| `unknown` | no probe (the search never ran at the net's own width, touched its window's edge, or was a zone connection), the gap takes `W` (so something else stopped the search), or the diagnostic budget (a deterministic number of obstacle tests per run) is used up | none: a failed search alone proves neither |

Measuring costs obstacle tests, so it is done at a connection's 1st, 2nd, 4th, 8th ... failure and reused in between (the path is always fresh).
Nothing is counted as work and nothing changes in the search: a run with a sink attached routes exactly as one without (tested: same copper, same number of
expansions).

