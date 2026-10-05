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
  | { type: 'stage'; name: string; state: 'begin' | 'end'; detail?: string }
  | { type: 'log'; level: string; text: string }
  // Escape planning (M9): reserved corridor per dense-package pin, released when the pin is connected; dead pins.
  | { type: 'escape_plan'; corridors: { id: number; net: number; pad: string; via: boolean; pts: XY[] }[] }
  | { type: 'escape_release'; id: number }
  | { type: 'escape_dead'; id: number; net: number; pad: string; p: XY; why: string };
