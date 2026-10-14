// SPDX-License-Identifier: GPL-3.0-or-later
// The stall picture (issue #8) on the 2D label canvas: the rejected paths of the selected connection, drawn until dismissed and coloured
// by cause, rings at its two pads, a mark at the place the way is narrowest (or the closest approach), and beside it the engine's numbers
// and the verdict. Plain canvas calls through a small interface so the drawing can be tested with a recording context under Node.
import { reasonColor, stallLines, verdictColor, type StallEntry } from './stall.ts';

export interface Canvas2D {
  save(): void;
  restore(): void;
  beginPath(): void;
  moveTo(x: number, y: number): void;
  lineTo(x: number, y: number): void;
  arc(x: number, y: number, r: number, a0: number, a1: number): void;
  stroke(): void;
  fill(): void;
  fillRect(x: number, y: number, w: number, h: number): void;
  strokeRect(x: number, y: number, w: number, h: number): void;
  fillText(text: string, x: number, y: number): void;
  measureText(text: string): { width: number };
  setLineDash(d: number[]): void;
  strokeStyle: string | CanvasGradient | CanvasPattern;
  fillStyle: string | CanvasGradient | CanvasPattern;
  lineWidth: number;
  lineCap: CanvasLineCap;
  lineJoin: CanvasLineJoin;
  font: string;
  textAlign: CanvasTextAlign;
  textBaseline: CanvasTextBaseline;
  globalAlpha: number;
}

export interface DrawEnv {
  /** Board nanometres to screen pixels. */
  toScreen(xnm: number, ynm: number): [number, number];
  width: number; // screen size in CSS pixels
  height: number;
  activeLayer: number;
  layerNames: string[];
  /** Left of this x the screen is free (the side panel is on the right: labels stay clear of it). */
  rightMargin: number;
}

/** Where the label goes: beside the anchor and clear of the line through it (the rejected path usually arrives along that line), above it
 *  when there is room, else below, and kept on the screen and off the side panel. Returns the box's top-left corner. */
export function placeLabel(ax: number, ay: number, w: number, h: number, env: Pick<DrawEnv, 'width' | 'height' | 'rightMargin'>): [number, number] {
  const maxX = env.width - env.rightMargin - w - 8;
  let x = ax + 14;
  if (x > maxX) x = ax - 14 - w; // no room on the right: put it on the left
  x = Math.min(Math.max(8, x), Math.max(8, maxX));
  const maxY = Math.max(8, env.height - h - 36);
  let y = ay - h - 16;
  if (y < 8) y = ay + 16;
  y = Math.min(Math.max(8, y), maxY);
  return [x, y];
}

export function drawStall(ctx: Canvas2D, e: StallEntry, env: DrawEnv): void {
  const color = reasonColor(e.reason);
  ctx.save();
  ctx.lineCap = 'round';
  // Every rejected path, newest brightest: kept on screen until dismissed, not a 4.5 s effect.
  e.paths.forEach((path, k) => {
    ctx.globalAlpha = 0.35 + 0.65 * ((k + 1) / e.paths.length);
    ctx.strokeStyle = color;
    ctx.lineWidth = k === e.paths.length - 1 ? 2.5 : 1.5;
    ctx.setLineDash([7, 5]);
    ctx.beginPath();
    path.forEach(([x, y], i) => {
      const [sx, sy] = env.toScreen(x, y);
      if (i === 0) ctx.moveTo(sx, sy);
      else ctx.lineTo(sx, sy);
    });
    ctx.stroke();
  });
  ctx.setLineDash([]);
  ctx.globalAlpha = 1;
  // The two ends of the connection.
  ctx.strokeStyle = color;
  ctx.lineWidth = 2;
  for (const p of [e.a, e.b]) {
    const [sx, sy] = env.toScreen(p[0], p[1]);
    ctx.beginPath();
    ctx.arc(sx, sy, 9, 0, Math.PI * 2);
    ctx.stroke();
  }
  // The place the way is narrowest, or where the search got closest.
  const last = e.paths.length ? e.paths[e.paths.length - 1] : null;
  const anchor = e.at ?? (last ? [last[last.length - 1][0], last[last.length - 1][1]] : e.a);
  const [ax, ay] = env.toScreen(anchor[0], anchor[1]);
  ctx.strokeStyle = '#ffffff';
  ctx.lineWidth = 2;
  ctx.beginPath();
  ctx.moveTo(ax - 8, ay);
  ctx.lineTo(ax + 8, ay);
  ctx.moveTo(ax, ay - 8);
  ctx.lineTo(ax, ay + 8);
  ctx.stroke();
  // The label, the verdict, and the rule it came from.
  const lines = stallLines(e, env.layerNames);
  ctx.font = '600 12px "Inter", system-ui, -apple-system, "Segoe UI", sans-serif';
  const rows: { text: string; color: string }[] = [{ text: lines.label, color: '#e8eefc' }, { text: lines.verdict, color: verdictColor(e.verdict) }];
  if (lines.rule) rows.push({ text: lines.rule, color: '#9aa7c7' });
  const pad = 8, lh = 17;
  let w = 0;
  for (const r of rows) w = Math.max(w, ctx.measureText(r.text).width);
  w += 2 * pad;
  const h = rows.length * lh + 2 * pad - 4;
  const [bx, by] = placeLabel(ax, ay, w, h, env);
  ctx.fillStyle = 'rgba(8, 12, 24, 0.92)';
  ctx.fillRect(bx, by, w, h);
  ctx.strokeStyle = verdictColor(e.verdict);
  ctx.lineWidth = 1.5;
  ctx.strokeRect(bx, by, w, h);
  ctx.textAlign = 'left';
  ctx.textBaseline = 'middle';
  rows.forEach((r, i) => {
    ctx.fillStyle = r.color;
    ctx.fillText(r.text, bx + pad, by + pad - 2 + lh * i + lh / 2);
  });
  ctx.restore();
}
