// SPDX-License-Identifier: GPL-3.0-or-later
// The reference-return overlay (issue #7) on the 2D label canvas: the signal in cyan, the return in red with arrows (from the end of the
// trace back to its start, shifted a few pixels so the two can be told apart where they run together), and for the selected net the
// callouts: signal pad, assumed return pad, signal via, return via or capacitor, and the separation. Plain canvas calls through a small
// interface so the drawing can be tested with a recording context under Node.
import type { Canvas2D } from './stall_draw.ts';
import type { ReturnPath } from './scene.ts';

export const SIGNAL_COLOR = '#33d6ff';
export const RETURN_COLOR = '#ff4d5e';

export interface RpEnv {
  toScreen(x: number, y: number): [number, number]; // world mm to screen px
  width: number;
  height: number;
  rightMargin: number;
}

/** Points of an arrow head at (x, y) pointing along (dx, dy), `size` px long. */
export function arrowHead(x: number, y: number, dx: number, dy: number, size: number): [number, number][] {
  const l = Math.hypot(dx, dy) || 1;
  const ux = dx / l, uy = dy / l;
  return [
    [x, y],
    [x - ux * size + uy * size * 0.5, y - uy * size - ux * size * 0.5],
    [x - ux * size - uy * size * 0.5, y - uy * size + ux * size * 0.5],
  ];
}

const CALLOUT_COLORS: Record<string, string> = {
  signal_pad: SIGNAL_COLOR, ref_pad: '#ffb454', signal_via: SIGNAL_COLOR, return_via: RETURN_COLOR, missing_stitch: '#ff5d6c', separation: '#ffd166', split: '#ff5d6c',
};

function polyline(ctx: Canvas2D, flat: number[], env: RpEnv, shift: number) {
  const pts: [number, number][] = [];
  for (let i = 0; i < flat.length; i += 2) pts.push(env.toScreen(flat[i], flat[i + 1]));
  if (pts.length < 2) return pts;
  ctx.beginPath();
  pts.forEach(([x, y], i) => {
    let ox = 0, oy = 0;
    if (shift) {
      // The shift is perpendicular to the way the polyline runs at this point.
      const a = pts[Math.max(0, i - 1)], b = pts[Math.min(pts.length - 1, i + 1)];
      const dx = b[0] - a[0], dy = b[1] - a[1], l = Math.hypot(dx, dy) || 1;
      ox = (-dy / l) * shift;
      oy = (dx / l) * shift;
    }
    if (i === 0) ctx.moveTo(x + ox, y + oy);
    else ctx.lineTo(x + ox, y + oy);
  });
  ctx.stroke();
  return pts;
}

/** Draws every net's lines; the selected net's with callouts. */
export function drawReturnPaths(ctx: Canvas2D, paths: Iterable<ReturnPath>, selected: string | null, env: RpEnv): void {
  ctx.save();
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  for (const p of paths) {
    const sel = p.name === selected;
    ctx.globalAlpha = selected && !sel ? 0.45 : 1;
    ctx.setLineDash([]);
    ctx.strokeStyle = SIGNAL_COLOR;
    ctx.lineWidth = sel ? 3.5 : 2.2;
    for (const s of p.signal) polyline(ctx, s, env, 0);
    ctx.strokeStyle = RETURN_COLOR;
    ctx.lineWidth = sel ? 3 : 1.8;
    for (const r of p.ret) {
      // Returning: from the last point back to the first, so the arrows point that way.
      const rev: number[] = [];
      for (let i = r.length - 2; i >= 0; i -= 2) rev.push(r[i], r[i + 1]);
      const pts = polyline(ctx, rev, env, 3);
      // An arrow head every ~70 px along it.
      ctx.fillStyle = RETURN_COLOR;
      let acc = 0;
      for (let i = 1; i < pts.length; i++) {
        const dx = pts[i][0] - pts[i - 1][0], dy = pts[i][1] - pts[i - 1][1], seg = Math.hypot(dx, dy);
        acc += seg;
        if (acc >= 70 && seg > 6) {
          acc = 0;
          const h = arrowHead((pts[i][0] + pts[i - 1][0]) / 2, (pts[i][1] + pts[i - 1][1]) / 2, dx, dy, sel ? 9 : 7);
          ctx.beginPath();
          ctx.moveTo(h[0][0], h[0][1]);
          ctx.lineTo(h[1][0], h[1][1]);
          ctx.lineTo(h[2][0], h[2][1]);
          ctx.fill();
        }
      }
    }
    if (!sel) continue;
    ctx.globalAlpha = 1;
    ctx.font = '600 11.5px "Inter", system-ui, -apple-system, "Segoe UI", sans-serif';
    ctx.textAlign = 'left';
    ctx.textBaseline = 'middle';
    // Labels in the order they are listed; one that would cover an earlier label is tried on the other sides of its marker, then left out
    // (its marker stays): a dense field must not turn into a smear of text.
    const placed: [number, number, number, number][] = [];
    const fits = (bx: number, by: number, w: number) =>
      bx >= 6 && by >= 6 && bx + w <= env.width - env.rightMargin - 6 && by + 18 <= env.height - 36 &&
      placed.every(([x, y, pw, ph]) => bx + w < x || bx > x + pw || by + 18 < y || by > y + ph);
    p.callouts.forEach((c) => {
      const [x, y] = env.toScreen(c.x, c.y);
      const color = CALLOUT_COLORS[c.kind] ?? '#e8eefc';
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath();
      ctx.arc(x, y, 6, 0, Math.PI * 2);
      ctx.stroke();
      const w = ctx.measureText(c.text).width + 12;
      const spots: [number, number][] = [[x + 10, y - 24], [x + 10, y + 8], [x - 10 - w, y - 24], [x - 10 - w, y + 8], [x + 10, y - 46], [x - 10 - w, y - 46]];
      const at = spots.find(([bx, by]) => fits(bx, by, w));
      if (!at) return;
      placed.push([at[0], at[1], w, 18]);
      ctx.fillStyle = 'rgba(8, 12, 24, 0.9)';
      ctx.fillRect(at[0], at[1], w, 18);
      ctx.strokeStyle = color;
      ctx.lineWidth = 1;
      ctx.strokeRect(at[0], at[1], w, 18);
      ctx.fillStyle = '#e8eefc';
      ctx.fillText(c.text, at[0] + 6, at[1] + 9);
    });
  }
  ctx.restore();
}

/** The world-space box of a net's lines (for centring the view on it): [x0, y0, x1, y1], or null. */
export function pathBox(p: ReturnPath): [number, number, number, number] | null {
  let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
  for (const s of [...p.signal, ...p.ret])
    for (let i = 0; i < s.length; i += 2) {
      x0 = Math.min(x0, s[i]); x1 = Math.max(x1, s[i]);
      y0 = Math.min(y0, s[i + 1]); y1 = Math.max(y1, s[i + 1]);
    }
  return x0 <= x1 ? [x0, y0, x1, y1] : null;
}
