// Small geometry helpers in world units (millimetres).

/** KiCad rotation (degrees, positive = counter-clockwise on screen in y-down coordinates). */
export function rotate(x: number, y: number, deg: number): [number, number] {
  const r = (deg * Math.PI) / 180;
  const c = Math.cos(r), s = Math.sin(r);
  return [x * c + y * s, -x * s + y * c];
}

export function distToSeg(px: number, py: number, ax: number, ay: number, bx: number, by: number): number {
  const dx = bx - ax, dy = by - ay;
  const l2 = dx * dx + dy * dy;
  let t = l2 > 0 ? ((px - ax) * dx + (py - ay) * dy) / l2 : 0;
  t = Math.max(0, Math.min(1, t));
  return Math.hypot(px - (ax + t * dx), py - (ay + t * dy));
}

export function pointInPolygon(px: number, py: number, pts: ArrayLike<number>): boolean {
  let inside = false;
  const n = pts.length / 2;
  for (let i = 0, j = n - 1; i < n; j = i++) {
    const xi = pts[2 * i], yi = pts[2 * i + 1], xj = pts[2 * j], yj = pts[2 * j + 1];
    if (yi > py !== yj > py && px < ((xj - xi) * (py - yi)) / (yj - yi) + xi) inside = !inside;
  }
  return inside;
}

/**
 * Ear-clipping triangulation of a simple polygon (flat [x0,y0,x1,y1,…], either winding).
 * Quadratic, intended for pad outlines (tens of vertices). Returns vertex index triples.
 * Reference: D. Eberly, "Triangulation by Ear Clipping" (2002).
 */
export function earClip(pts: ArrayLike<number>): number[] {
  let n = pts.length / 2;
  if (n >= 2 && pts[0] === pts[2 * n - 2] && pts[1] === pts[2 * n - 1]) n--; // closing duplicate
  if (n < 3) return [];
  let area = 0;
  for (let i = 0, j = n - 1; i < n; j = i++) area += pts[2 * j] * pts[2 * i + 1] - pts[2 * i] * pts[2 * j + 1];
  const idx: number[] = [];
  for (let i = 0; i < n; i++) idx.push(area > 0 ? i : n - 1 - i); // make counter-clockwise (in y-up sense)
  const out: number[] = [];
  const x = (i: number) => pts[2 * i], y = (i: number) => pts[2 * i + 1];
  const cross = (a: number, b: number, c: number) => (x(b) - x(a)) * (y(c) - y(a)) - (y(b) - y(a)) * (x(c) - x(a));
  const inTri = (p: number, a: number, b: number, c: number) =>
    cross(a, b, p) >= 0 && cross(b, c, p) >= 0 && cross(c, a, p) >= 0;
  let guard = 0;
  while (idx.length > 3 && guard++ < 10000) {
    let clipped = false;
    for (let k = 0; k < idx.length; k++) {
      const a = idx[(k + idx.length - 1) % idx.length], b = idx[k], c = idx[(k + 1) % idx.length];
      if (cross(a, b, c) <= 0) continue; // reflex or degenerate
      let ear = true;
      for (const p of idx) {
        if (p === a || p === b || p === c) continue;
        if (inTri(p, a, b, c)) { ear = false; break; }
      }
      if (!ear) continue;
      out.push(a, b, c);
      idx.splice(k, 1);
      clipped = true;
      break;
    }
    if (!clipped) break; // degenerate input: fall back to a fan for the rest
  }
  if (idx.length >= 3) for (let k = 1; k + 1 < idx.length; k++) out.push(idx[0], idx[k], idx[k + 1]);
  return out;
}
