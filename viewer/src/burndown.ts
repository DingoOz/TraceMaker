// SPDX-License-Identifier: GPL-3.0-or-later
// Burndown of the open connections (the `stats` stream, docs/13-viewer-protocol.md): how many connections are still unrouted
// against the engine's elapsed time, with the stages marked where they began. Kept apart from the HUD so it runs under Node.

export interface BurnPoint { t: number; open: number; routed: number }
export interface BurnMark { t: number; name: string }

/** The history of one run. A stats message whose time goes backwards, or whose total changes, starts a new run (a restart,
 *  another board, or a recording replayed from the start). Long runs keep at most `max` points by dropping every other one. */
export class Burndown {
  points: BurnPoint[] = [];
  marks: BurnMark[] = [];
  total = 0;
  readonly max: number;
  constructor(max = 1200) {
    this.max = max;
  }

  reset() {
    this.points = [];
    this.marks = [];
    this.total = 0;
  }

  add(t: number, routed: number, total: number, open: number) {
    const last = this.points[this.points.length - 1];
    if (last && (total !== this.total || t < last.t)) this.reset();
    this.total = total;
    const p = { t, open, routed };
    if (last && last.t === t) this.points[this.points.length - 1] = p;
    else this.points.push(p);
    if (this.points.length > this.max) this.points = this.points.filter((_, i) => i % 2 === 0 || i === this.points.length - 1);
  }

  /** The history the server kept (`stats_history`): [t, routed, total, open] points and [t, stage] marks. */
  load(points: [number, number, number, number][], marks: [number, string][]) {
    this.reset();
    for (const [t, routed, total, open] of points) this.add(t, routed, total, open);
    this.marks = marks.map(([t, name]) => ({ t, name }));
  }

  /** A stage began; it is drawn at the time of the latest stats message (the start before there is one). */
  mark(name: string) {
    const last = this.points[this.points.length - 1];
    const prev = this.marks[this.marks.length - 1];
    if (prev && prev.name === name) return;
    this.marks.push({ t: last ? last.t : 0, name });
  }
}

const esc = (s: string) => s.replace(/[&<>"]/g, (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;' })[c]!);
const f1 = (v: number) => (Math.round(v * 10) / 10).toString();

/** The chart as SVG markup, `w` by `h` pixels: the open connections as a filled step line from the total down to zero, time
 *  along the bottom from 0 to the latest point, stage starts as dashed lines with their names in the tooltip. */
export function burndownSvg(b: Burndown, w: number, h: number): string {
  const padL = 34, padR = 6, padT = 6, padB = 16;
  const iw = w - padL - padR, ih = h - padT - padB;
  const pts = b.points;
  const tmax = Math.max(1, pts.length ? pts[pts.length - 1].t : 1);
  const ymax = Math.max(1, b.total);
  const x = (t: number) => padL + (iw * t) / tmax;
  const y = (v: number) => padT + ih - (ih * v) / ymax;
  const out: string[] = [`<svg class="burn-svg" viewBox="0 0 ${w} ${h}" width="${w}" height="${h}" role="img" aria-label="Open connections over time">`];
  // Axes: the total and zero on the left, the start and end time along the bottom.
  out.push(`<line class="burn-axis" x1="${padL}" y1="${padT + ih}" x2="${padL + iw}" y2="${padT + ih}"/>`);
  out.push(`<line class="burn-grid" x1="${padL}" y1="${padT}" x2="${padL + iw}" y2="${padT}"/>`);
  out.push(`<text class="burn-tick" x="${padL - 4}" y="${padT + 4}" text-anchor="end">${ymax}</text>`);
  out.push(`<text class="burn-tick" x="${padL - 4}" y="${padT + ih}" text-anchor="end">0</text>`);
  out.push(`<text class="burn-tick" x="${padL}" y="${h - 3}">0 s</text>`);
  out.push(`<text class="burn-tick" x="${padL + iw}" y="${h - 3}" text-anchor="end">${esc(clock(tmax))}</text>`);
  for (const m of b.marks) {
    const mx = f1(x(m.t));
    out.push(`<line class="burn-mark" x1="${mx}" y1="${padT}" x2="${mx}" y2="${padT + ih}"><title>${esc(m.name)} at ${esc(clock(m.t))}</title></line>`);
  }
  if (pts.length) {
    // Steps: the count holds until the next message changes it.
    let d = `M${f1(x(pts[0].t))},${f1(y(pts[0].open))}`;
    for (let i = 1; i < pts.length; i++) d += `H${f1(x(pts[i].t))}V${f1(y(pts[i].open))}`;
    const lastX = f1(x(pts[pts.length - 1].t));
    out.push(`<path class="burn-area" d="${d}V${f1(padT + ih)}H${f1(x(pts[0].t))}Z"/>`);
    out.push(`<path class="burn-line" d="${d}"/>`);
    const end = pts[pts.length - 1];
    out.push(`<circle class="burn-dot" cx="${lastX}" cy="${f1(y(end.open))}" r="2.5"/>`);
  }
  out.push('</svg>');
  return out.join('');
}

function clock(s: number): string {
  if (s < 60) return `${Math.round(s)} s`;
  const m = Math.floor(s / 60), r = Math.round(s % 60);
  return m < 60 ? `${m}:${String(r).padStart(2, '0')}` : `${Math.floor(m / 60)}:${String(m % 60).padStart(2, '0')}:${String(r).padStart(2, '0')}`;
}
