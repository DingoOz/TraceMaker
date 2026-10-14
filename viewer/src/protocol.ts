// SPDX-License-Identifier: GPL-3.0-or-later
// Viewer protocol v1 (docs/13-viewer-protocol.md). Coordinates are integer nanometres.
export type XY = [number, number];

export interface PadMsg {
  id: number;
  net: number;
  layers: number[];
  shape: 'circle' | 'segment' | 'polygon';
  pts: XY[];
  r: number;
  fp?: number;
  num?: string;
  hole?: { pts: XY[]; r: number };
}
export interface TrackMsg { id: number; a: XY; b: XY; w: number; layer: number; net: number }
export interface ViaMsg { id: number; p: XY; d: number; drill: number; net: number; top: number; bottom: number }
export interface ZoneMsg { layer: number; net: number; polys: XY[][] }
export interface FootprintMsg { ref: string; x: number; y: number; angle: number; back: boolean; bbox: [number, number, number, number]; value?: string }

export interface BoardMsg {
  type: 'board';
  name: string;
  bbox: [number, number, number, number];
  layers: { name: string; index: number }[];
  nets: string[];
  outline: XY[][];
  pads: PadMsg[];
  tracks: TrackMsg[];
  vias: ViaMsg[];
  zones: ZoneMsg[];
  footprints: FootprintMsg[];
  /** `false` when the engine was started with --no-freeze: the viewer does not hold the scene on a stall. */
  freeze?: boolean;
}

export interface StatsMsg {
  type: 'stats';
  stage: string;
  iteration: number;
  routed: number;
  total: number;
  unrouted: number;
  rips: number;
  failures: number;
  elapsed_s: number;
  extra?: Record<string, number>;
}

export interface FailureMsg {
  type: 'failure';
  conn: number;
  net: number;
  rung: number;
  cause: string;
  a: XY;
  b: XY;
  blockers?: number[];
  region?: [number, number, number, number];
  // Issue #8 (docs/13-viewer-protocol.md "The failure verdict"): all optional, lengths in nm. Older engines send none of them.
  blocker_nets?: number[];
  reason?: string;
  width_nm?: number;
  clearance_nm?: number;
  min_width_nm?: number;
  gap_nm?: number;
  gap_fixed_nm?: number;
  gap_layer?: number;
  at?: XY;
  verdict?: 'placement' | 'rule' | 'contested' | 'unknown';
  rule?: string;
  rejected?: [number, number, number][];
}

/** One net's reference-return audit (docs/13-viewer-protocol.md `return_path`, issue #7): the signal, the return along it (the trace over
 *  copper, the detour across a void), and callouts. Polylines are in nm; the return is drawn from the chain's end to its start. */
export interface ReturnPathMsg {
  type: 'return_path';
  net: number;
  name: string;
  detour_mm: number;
  max_separation_mm: number;
  voids: number;
  splits: number;
  missing_stitches: number;
  signal: { layer: number; pts: XY[] }[];
  return: { layer: number; pts: XY[] }[];
  callouts: { kind: string; p: XY; text: string }[];
}

/** The footprint check against datasheet land patterns (docs/13-viewer-protocol.md `footprint_check`, issue #5): the footprints with something to
 *  say (a discrepancy, an untrusted reference, a missing required reference) and where each finding is. */
export interface FootprintCheckMsg {
  type: 'footprint_check';
  checked: number;
  ok: number;
  mismatch: number;
  unverified: number;
  untrusted: number;
  errors: number;
  warnings: number;
  footprints: {
    ref: string;
    lib_id: string;
    status: string;
    p: XY;
    datasheet?: { id: string; brand: string; part: string; document: string; revision?: string; page?: number; figure?: string; url?: string };
    issues: { severity: string; kind: string; pad: string; text: string; label?: string; p: XY }[];
  }[];
}

/** The critical nets after a route with --critical-* / --si-rules (docs/13-viewer-protocol.md `critical_nets`, issue #24): per net its tier, vias added,
 *  length added and, where the board has a reference plane, the voids of it that the net runs over. */
export interface CriticalNetsMsg {
  type: 'critical_nets';
  nets: { name: string; tier: number; why: string; vias: number; length_mm: number; unrouted?: number; void_crossings?: number; detour_mm?: number; splits?: number; warnings: string[] }[];
  totals: { nets: number; vias: number; void_crossings: number; over_via_limit: number };
}

/** Overlay grid (doc 13): row-major w*h bytes 0–255, cell size in nm, layer -1 = all layers. `scale` says how
 *  raw values became bytes ("sqrt": byte = 255·sqrt(v / max); absent: linear). */
export interface HeatmapMsg {
  type: 'heatmap';
  name: string;
  x0: number;
  y0: number;
  cell: number;
  w: number;
  h: number;
  layer: number;
  max: number;
  scale?: string;
  data: number[];
}

export type Message =
  | BoardMsg
  | StatsMsg
  | FailureMsg
  | { type: 'track_add'; track: TrackMsg }
  | { type: 'track_remove'; id: number }
  | { type: 'via_add'; via: ViaMsg }
  | { type: 'via_remove'; id: number }
  | { type: 'footprint_move'; ref: string; x: number; y: number; angle: number }
  | { type: 'ratsnest'; edges: [number, number, number, number, number][] }
  | { type: 'frontier'; conn: number; layer: number; pts: XY[] }
  | { type: 'path_try'; conn: number; pts: [number, number, number][] }
  | HeatmapMsg
  | ReturnPathMsg
  | FootprintCheckMsg
  | CriticalNetsMsg
  | { type: 'stage'; name: string; state: 'begin' | 'end'; detail?: string }
  | { type: 'log'; level: string; text: string }
  // Escape planning (M9): reserved corridor per dense-package pin, released when the pin is connected; dead pins.
  | { type: 'escape_plan'; corridors: { id: number; net: number; pad: string; via: boolean; pts: XY[] }[] }
  | { type: 'escape_release'; id: number }
  | { type: 'escape_dead'; id: number; net: number; pad: string; p: XY; why: string };
