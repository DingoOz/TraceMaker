// SPDX-License-Identifier: GPL-3.0-or-later
// The footprint-check overlay (issue #5) on the 2D label canvas: a ring on every footprint that differs from its datasheet (red), has an untrusted
// reference or lacks a required one (amber), and for the selected footprint a mark on each pad or place that is wrong with the message beside it.
// Plain canvas calls through a small interface so the drawing can be tested with a recording context under Node.
import type { Canvas2D } from './stall_draw.ts';
import type { FootprintFinding } from './scene.ts';

export const ERROR_COLOR = '#ff4d5e';
export const WARNING_COLOR = '#ffb454';

export interface FcEnv {
  toScreen(x: number, y: number): [number, number]; // world mm to screen px
  width: number;
  height: number;
  rightMargin: number;
}

/** Red if any finding of the footprint is an error, amber otherwise. */
export function findingColor(f: FootprintFinding): string {
  return f.issues.some((i) => i.severity === 'error') ? ERROR_COLOR : WARNING_COLOR;
}

/** Draws every finding's ring; the selected footprint's issues with their messages. */
export function drawFootprintCheck(ctx: Canvas2D, findings: Iterable<FootprintFinding>, selected: string | null, env: FcEnv): void {
  ctx.save();
  ctx.lineCap = 'round';
  ctx.lineJoin = 'round';
  ctx.setLineDash([]);
  const chosen: FootprintFinding[] = [];
  for (const f of findings) {
    const sel = f.ref === selected;
    if (sel) chosen.push(f);
    const [x, y] = env.toScreen(f.x, f.y);
    if (x < -30 || y < -30 || x > env.width + 30 || y > env.height + 30) continue;
    ctx.globalAlpha = selected && !sel ? 0.5 : 1;
    ctx.strokeStyle = findingColor(f);
    ctx.lineWidth = sel ? 3 : 2;
    ctx.beginPath();
    ctx.arc(x, y, sel ? 16 : 11, 0, Math.PI * 2);
    ctx.stroke();
  }
  ctx.globalAlpha = 1;
  ctx.font = '600 11.5px "Inter", system-ui, -apple-system, "Segoe UI", sans-serif';
  ctx.textAlign = 'left';
  ctx.textBaseline = 'middle';
  // Labels in the order the issues are listed; one that would cover an earlier label is tried on the other sides of its mark, then left out (its
  // mark stays): a footprint with a dozen wrong pads must not turn into a smear of text.
  const placed: [number, number, number, number][] = [];
  const fits = (bx: number, by: number, w: number) =>
    bx >= 6 && by >= 6 && bx + w <= env.width - env.rightMargin - 6 && by + 18 <= env.height - 36 &&
    placed.every(([x, y, pw, ph]) => bx + w < x || bx > x + pw || by + 18 < y || by > y + ph);
  for (const f of chosen) {
    for (const i of f.issues) {
      const [x, y] = env.toScreen(i.x, i.y);
      const color = i.severity === 'error' ? ERROR_COLOR : WARNING_COLOR;
      ctx.strokeStyle = color;
      ctx.lineWidth = 2;
      ctx.beginPath(); // a cross: the pad, or the footprint as a whole
      ctx.moveTo(x - 6, y - 6);
      ctx.lineTo(x + 6, y + 6);
      ctx.moveTo(x - 6, y + 6);
      ctx.lineTo(x + 6, y - 6);
      ctx.stroke();
      const text = i.label.length > 60 ? `${i.label.slice(0, 59)}…` : i.label; // the full sentence is in the side panel
      const w = ctx.measureText(text).width + 12;
      const spots: [number, number][] = [[x + 10, y - 24], [x + 10, y + 8], [x - 10 - w, y - 24], [x - 10 - w, y + 8], [x + 10, y - 46], [x - 10 - w, y - 46], [x + 10, y + 30]];
      const at = spots.find(([bx, by]) => fits(bx, by, w));
      if (!at) continue;
      placed.push([at[0], at[1], w, 18]);
      ctx.fillStyle = 'rgba(8, 12, 24, 0.9)';
      ctx.fillRect(at[0], at[1], w, 18);
      ctx.strokeStyle = color;
      ctx.lineWidth = 1;
      ctx.strokeRect(at[0], at[1], w, 18);
      ctx.fillStyle = '#e8eefc';
      ctx.fillText(text, at[0] + 6, at[1] + 9);
    }
  }
  ctx.restore();
}

/** The world-space box of a footprint's finding and its marks (for centring the view): [x0, y0, x1, y1]. */
export function findingBox(f: FootprintFinding): [number, number, number, number] {
  let x0 = f.x, y0 = f.y, x1 = f.x, y1 = f.y;
  for (const i of f.issues) {
    x0 = Math.min(x0, i.x); x1 = Math.max(x1, i.x);
    y0 = Math.min(y0, i.y); y1 = Math.max(y1, i.y);
  }
  return [x0, y0, x1, y1];
}
