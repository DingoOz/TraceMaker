// Scene state in world units (millimetres relative to the board centre), updated by protocol messages.
import { earClip, rotate } from './geometry';
import type { BoardMsg, FootprintMsg, Message, PadMsg, TrackMsg, ViaMsg, XY } from './protocol';

export interface Track { id: number; ax: number; ay: number; bx: number; by: number; r: number; layer: number; net: number }
export interface Via { id: number; x: number; y: number; r: number; rh: number; net: number; top: number; bottom: number }
export interface Pad {
  id: number;
  net: number;
  layers: number[];
  shape: 'circle' | 'segment' | 'polygon';
  pts: number[]; // flat x,y
  r: number;
  fp: number;
  num: string;
  hole: { pts: number[]; r: number } | null;
  tris: number[] | null; // polygon triangulation (indices into pts)
}
export interface Footprint { ref: string; value: string; x: number; y: number; angle: number; back: boolean; bbox: [number, number, number, number] }
export interface Zone { layer: number; net: number; polys: number[][] }

/** Effects the renderer animates; times are performance.now() seconds. */
export interface Fx {
  newTracks: { t: Track; t0: number }[];
  frontier: { x: number; y: number; layer: number; t0: number }[];
  paths: { pts: number[]; t0: number }[];
  failures: { ax: number; ay: number; bx: number; by: number; t0: number }[];
}

export interface Activity { x: number; y: number; t: number }

const MM = 1e-6;

export class Scene {
  name = '';
  loaded = false;
  ox = 0; // board centre in nm
  oy = 0;
  bbox: [number, number, number, number] = [-50, -50, 50, 50]; // world
  layerNames: string[] = [];
  nets: string[] = [];
  outline: number[][] = [];
  pads: Pad[] = [];
  footprints: Footprint[] = [];
  zones: Zone[] = [];
  tracks = new Map<number, Track>();
  vias = new Map<number, Via>();
  ratsnest: number[] = []; // ax,ay,bx,by per edge
  ratsNets: number[] = [];

  // Change tracking for the renderer and the picker.
  version = 0; // bumps on any visible change
  boardVersion = 0; // bumps on a new snapshot
  dirtyTrackLayers = new Set<number>();
  viasDirty = true;
  padsDirty = true;
  ratsDirty = true;
  escapes = new Map<number, { pts: number[]; net: number; via: boolean }>();  // pad id -> corridor polyline (mm)
  deadPins: { x: number; y: number; net: number }[] = [];
  escDirty = true;
  footprintsDirty = true;

  fx: Fx = { newTracks: [], frontier: [], paths: [], failures: [] };
  activity: Activity | null = null;
  glowEnabled = true;
  /** Effects are suppressed while a client catches up (snapshot replay) or after a stall. */
  quietUntil = 0;

  wx(nm: number) { return (nm - this.ox) * MM; }
  wy(nm: number) { return (nm - this.oy) * MM; }
  private pt(p: XY): [number, number] { return [this.wx(p[0]), this.wy(p[1])]; }

  netName(n: number): string {
    return n > 0 && n < this.nets.length ? this.nets[n] || `net ${n}` : n === 0 ? '(no net)' : `net ${n}`;
  }

  /** Applies one message. Returns true if it was a log-worthy message for the HUD (handled by the caller). */
  apply(m: Message, now: number): void {
    switch (m.type) {
      case 'board': this.load(m); break;
      case 'track_add': {
        if (!this.loaded) return;
        const t = this.track(m.track);
        const old = this.tracks.get(t.id);
        if (old) this.dirtyTrackLayers.add(old.layer);
        this.tracks.set(t.id, t);
        this.dirtyTrackLayers.add(t.layer);
        if (this.glowEnabled && now >= this.quietUntil) this.fx.newTracks.push({ t, t0: now });
        this.activity = { x: (t.ax + t.bx) / 2, y: (t.ay + t.by) / 2, t: now };
        break;
      }
      case 'track_remove': {
        const t = this.tracks.get(m.id);
        if (t) {
          this.tracks.delete(m.id);
          this.dirtyTrackLayers.add(t.layer);
        }
        break;
      }
      case 'via_add': {
        if (!this.loaded) return;
        const v = this.via(m.via);
        this.vias.set(v.id, v);
        this.viasDirty = true;
        break;
      }
      case 'via_remove':
        if (this.vias.delete(m.id)) this.viasDirty = true;
        break;
      case 'footprint_move': this.moveFootprint(m.ref, m.x, m.y, m.angle); break;
      case 'ratsnest': {
        const r: number[] = [], nets: number[] = [];
        for (const e of m.edges) {
          r.push(this.wx(e[0]), this.wy(e[1]), this.wx(e[2]), this.wy(e[3]));
          nets.push(e[4] ?? 0);
        }
        this.ratsnest = r;
        this.ratsNets = nets;
        this.ratsDirty = true;
        break;
      }
      case 'frontier': {
        if (!this.loaded) return;
        let sx = 0, sy = 0;
        for (const p of m.pts) {
          const x = this.wx(p[0]), y = this.wy(p[1]);
          this.fx.frontier.push({ x, y, layer: m.layer, t0: now });
          sx += x;
          sy += y;
        }
        if (m.pts.length && m.conn >= 0) this.activity = { x: sx / m.pts.length, y: sy / m.pts.length, t: now };
        break;
      }
      case 'path_try': {
        if (!this.loaded) return;
        const pts: number[] = [];
        for (const p of m.pts) pts.push(this.wx(p[0]), this.wy(p[1]));
        this.fx.paths.push({ pts, t0: now });
        break;
      }
      case 'escape_plan': {
        this.escapes.clear();
        for (const c of m.corridors) this.escapes.set(c.id, { pts: c.pts.flatMap((p) => this.pt(p)), net: c.net, via: c.via });
        this.escDirty = true;
        break;
      }
      case 'escape_release':
        if (this.escapes.delete(m.id)) this.escDirty = true;
        break;
      case 'escape_dead': {
        const [x, y] = this.pt(m.p);
        this.deadPins.push({ x, y, net: m.net });
        this.escapes.delete(m.id);
        this.escDirty = true;
        break;
      }
      case 'failure': {
        if (!this.loaded) return;
        const [ax, ay] = this.pt(m.a), [bx, by] = this.pt(m.b);
        if (now >= this.quietUntil) this.fx.failures.push({ ax, ay, bx, by, t0: now });
        this.activity = { x: (ax + bx) / 2, y: (ay + by) / 2, t: now };
        break;
      }
      default: return;
    }
    this.version++;
  }

  private track(t: TrackMsg): Track {
    const [ax, ay] = this.pt(t.a), [bx, by] = this.pt(t.b);
    return { id: t.id, ax, ay, bx, by, r: (t.w * MM) / 2, layer: t.layer, net: t.net };
  }

  private via(v: ViaMsg): Via {
    const [x, y] = this.pt(v.p);
    return { id: v.id, x, y, r: (v.d * MM) / 2, rh: (v.drill * MM) / 2, net: v.net, top: v.top, bottom: v.bottom };
  }

  private pad(p: PadMsg): Pad {
    const pts: number[] = [];
    for (const q of p.pts) pts.push(this.wx(q[0]), this.wy(q[1]));
    let hole: Pad['hole'] = null;
    if (p.hole) {
      const hp: number[] = [];
      for (const q of p.hole.pts) hp.push(this.wx(q[0]), this.wy(q[1]));
      hole = { pts: hp, r: p.hole.r * MM };
    }
    return {
      id: p.id, net: p.net, layers: p.layers, shape: p.shape, pts, r: p.r * MM, fp: p.fp ?? -1, num: p.num ?? '',
      hole, tris: p.shape === 'polygon' ? earClip(pts) : null,
    };
  }

  private footprint(f: FootprintMsg): Footprint {
    return {
      ref: f.ref, value: f.value ?? '', x: this.wx(f.x), y: this.wy(f.y), angle: f.angle, back: f.back,
      bbox: [this.wx(f.bbox[0]), this.wy(f.bbox[1]), this.wx(f.bbox[2]), this.wy(f.bbox[3])],
    };
  }

  private load(b: BoardMsg) {
    this.name = b.name;
    this.ox = (b.bbox[0] + b.bbox[2]) / 2;
    this.oy = (b.bbox[1] + b.bbox[3]) / 2;
    this.bbox = [this.wx(b.bbox[0]), this.wy(b.bbox[1]), this.wx(b.bbox[2]), this.wy(b.bbox[3])];
    const layers = [...b.layers].sort((p, q) => p.index - q.index);
    this.layerNames = layers.map((l) => l.name);
    this.nets = b.nets;
    this.outline = b.outline.map((loop) => loop.flatMap((p) => this.pt(p)));
    this.pads = b.pads.map((p) => this.pad(p));
    this.footprints = b.footprints.map((f) => this.footprint(f));
    this.zones = b.zones.map((z) => ({ layer: z.layer, net: z.net, polys: z.polys.map((poly) => poly.flatMap((p) => this.pt(p))) }));
    this.tracks.clear();
    for (const t of b.tracks) this.tracks.set(t.id, this.track(t));
    this.vias.clear();
    for (const v of b.vias) this.vias.set(v.id, this.via(v));
    this.ratsnest = [];
    this.ratsNets = [];
    this.escapes.clear();
    this.deadPins = [];
    this.escDirty = true;
    this.fx = { newTracks: [], frontier: [], paths: [], failures: [] };
    this.activity = null;
    for (let i = 0; i < this.layerNames.length; i++) this.dirtyTrackLayers.add(i);
    this.viasDirty = this.padsDirty = this.ratsDirty = this.footprintsDirty = true;
    this.loaded = true;
    this.boardVersion++;
  }

  private moveFootprint(ref: string, xnm: number, ynm: number, angle: number) {
    const fi = this.footprints.findIndex((f) => f.ref === ref);
    if (fi < 0) return;
    const f = this.footprints[fi];
    const nx = this.wx(xnm), ny = this.wy(ynm), da = angle - f.angle;
    const tf = (arr: number[]) => {
      for (let i = 0; i < arr.length; i += 2) {
        const [rx, ry] = rotate(arr[i] - f.x, arr[i + 1] - f.y, da);
        arr[i] = nx + rx;
        arr[i + 1] = ny + ry;
      }
    };
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const p of this.pads) {
      if (p.fp !== fi) continue;
      tf(p.pts);
      if (p.hole) tf(p.hole.pts);
      for (let i = 0; i < p.pts.length; i += 2) {
        x0 = Math.min(x0, p.pts[i] - p.r); x1 = Math.max(x1, p.pts[i] + p.r);
        y0 = Math.min(y0, p.pts[i + 1] - p.r); y1 = Math.max(y1, p.pts[i + 1] + p.r);
      }
    }
    f.x = nx;
    f.y = ny;
    f.angle = angle;
    f.bbox = x0 <= x1 ? [x0, y0, x1, y1] : [nx, ny, nx, ny];
    this.padsDirty = this.footprintsDirty = true;
    this.activity = { x: nx, y: ny, t: performance.now() / 1000 };
  }
}
