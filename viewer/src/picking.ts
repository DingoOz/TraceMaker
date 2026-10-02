// CPU picking for hover: a uniform grid over item bounding boxes, rebuilt lazily when the scene changes.
import { distToSeg, pointInPolygon } from './geometry';
import type { Pad, Scene, Track, Via } from './scene';

export type Hit =
  | { kind: 'track'; item: Track }
  | { kind: 'via'; item: Via }
  | { kind: 'pad'; item: Pad };

interface Entry { hit: Hit; x0: number; y0: number; x1: number; y1: number }

export class Picker {
  private cells = new Map<number, Entry[]>();
  private cell = 1;
  private x0 = 0;
  private y0 = 0;
  private cols = 1;
  private version = -1;
  private board = -1;
  private built = 0;

  private key(cx: number, cy: number) { return cy * this.cols + cx; }

  private rebuild(scene: Scene) {
    this.version = scene.version;
    this.board = scene.boardVersion;
    this.built = performance.now();
    this.cells.clear();
    const [bx0, by0, bx1, by1] = scene.bbox;
    const span = Math.max(bx1 - bx0, by1 - by0, 1);
    this.cell = Math.max(span / 256, 0.25);
    this.x0 = bx0 - 10;
    this.y0 = by0 - 10;
    this.cols = Math.ceil((bx1 - bx0 + 20) / this.cell) + 1;
    const add = (hit: Hit, x0: number, y0: number, x1: number, y1: number) => {
      const e = { hit, x0, y0, x1, y1 };
      const c0 = Math.max(0, Math.floor((x0 - this.x0) / this.cell)), c1 = Math.min(this.cols - 1, Math.floor((x1 - this.x0) / this.cell));
      const r0 = Math.max(0, Math.floor((y0 - this.y0) / this.cell)), r1 = Math.floor((y1 - this.y0) / this.cell);
      for (let r = r0; r <= r1; r++)
        for (let c = c0; c <= c1; c++) {
          const k = this.key(c, r);
          let list = this.cells.get(k);
          if (!list) this.cells.set(k, (list = []));
          list.push(e);
        }
    };
    for (const t of scene.tracks.values())
      add({ kind: 'track', item: t }, Math.min(t.ax, t.bx) - t.r, Math.min(t.ay, t.by) - t.r, Math.max(t.ax, t.bx) + t.r, Math.max(t.ay, t.by) + t.r);
    for (const v of scene.vias.values()) add({ kind: 'via', item: v }, v.x - v.r, v.y - v.r, v.x + v.r, v.y + v.r);
    for (const p of scene.pads) {
      let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
      for (let i = 0; i < p.pts.length; i += 2) {
        x0 = Math.min(x0, p.pts[i]); x1 = Math.max(x1, p.pts[i]);
        y0 = Math.min(y0, p.pts[i + 1]); y1 = Math.max(y1, p.pts[i + 1]);
      }
      add({ kind: 'pad', item: p }, x0 - p.r, y0 - p.r, x1 + p.r, y1 + p.r);
    }
  }

  /** Topmost item under (x, y) on visible layers; `tol` is the pick tolerance in mm. */
  pick(scene: Scene, x: number, y: number, tol: number, visible: boolean[], active: number): Hit | null {
    if (!scene.loaded) return null;
    // During live routing the scene changes every frame: rebuild at most a few times per second.
    if (this.board !== scene.boardVersion || (this.version !== scene.version && performance.now() - this.built > 400)) this.rebuild(scene);
    const c = Math.floor((x - this.x0) / this.cell), r = Math.floor((y - this.y0) / this.cell);
    let best: Hit | null = null, bestScore = Infinity;
    const seen = new Set<Entry>();
    for (let dr = -1; dr <= 1; dr++)
      for (let dc = -1; dc <= 1; dc++) {
        const list = this.cells.get(this.key(c + dc, r + dr));
        if (!list) continue;
        for (const e of list) {
          if (seen.has(e) || x < e.x0 - tol || x > e.x1 + tol || y < e.y0 - tol || y > e.y1 + tol) continue;
          seen.add(e);
          const s = score(e.hit, x, y, tol, visible, active);
          if (s < bestScore) { bestScore = s; best = e.hit; }
        }
      }
    return best;
  }
}

// Lower is better: distance outside the shape, with priority bands (vias, pads, active layer, others).
function score(h: Hit, x: number, y: number, tol: number, visible: boolean[], active: number): number {
  let d: number, band: number;
  if (h.kind === 'track') {
    const t = h.item;
    if (!visible[t.layer]) return Infinity;
    d = distToSeg(x, y, t.ax, t.ay, t.bx, t.by) - t.r;
    band = t.layer === active ? 2 : 3;
  } else if (h.kind === 'via') {
    d = Math.hypot(x - h.item.x, y - h.item.y) - h.item.r;
    band = 0;
  } else {
    const p = h.item;
    if (p.layers.length && !p.layers.some((l) => visible[l])) return Infinity;
    const q = p.pts;
    if (p.shape === 'circle') d = Math.hypot(x - q[0], y - q[1]) - p.r;
    else if (p.shape === 'segment') d = distToSeg(x, y, q[0], q[1], q[2], q[3]) - p.r;
    else if (pointInPolygon(x, y, q)) d = -1;
    else {
      d = Infinity;
      const n = q.length / 2;
      for (let i = 0; i < n; i++) {
        const j = (i + 1) % n;
        d = Math.min(d, distToSeg(x, y, q[2 * i], q[2 * i + 1], q[2 * j], q[2 * j + 1]) - p.r);
      }
    }
    band = 1;
  }
  if (d > tol) return Infinity;
  return band * 1e6 + Math.max(d, 0);
}
