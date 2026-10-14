// SPDX-License-Identifier: GPL-3.0-or-later
// Run: node --experimental-strip-types --test src/returnpath_draw.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { arrowHead, drawReturnPaths, pathBox, RETURN_COLOR, SIGNAL_COLOR } from './returnpath_draw.ts';
import type { Canvas2D } from './stall_draw.ts';
import type { ReturnPath } from './scene.ts';

function recorder() {
  const calls: { op: string; args: unknown[]; stroke: string; fill: string; alpha: number; width: number }[] = [];
  const ctx: Record<string, unknown> = { strokeStyle: '', fillStyle: '', lineWidth: 1, font: '', textAlign: 'left', textBaseline: 'alphabetic', globalAlpha: 1, lineCap: 'butt', lineJoin: 'miter' };
  for (const op of ['save', 'restore', 'beginPath', 'moveTo', 'lineTo', 'arc', 'stroke', 'fill', 'fillRect', 'strokeRect', 'fillText', 'setLineDash'])
    ctx[op] = (...args: unknown[]) => calls.push({ op, args, stroke: String(ctx.strokeStyle), fill: String(ctx.fillStyle), alpha: Number(ctx.globalAlpha), width: Number(ctx.lineWidth) });
  ctx.measureText = (t: string) => ({ width: t.length * 6.5 });
  return { ctx: ctx as unknown as Canvas2D, calls };
}
const env = { toScreen: (x: number, y: number): [number, number] => [x * 20 + 50, y * 20 + 50], width: 1200, height: 800, rightMargin: 340 };

const net = (name: string, extra: Partial<ReturnPath> = {}): ReturnPath => ({
  name, net: 1, detourMm: 1, maxSeparationMm: 0.5, voids: 1, splits: 0, missingStitches: 0,
  signal: [[0, 0, 20, 0]], ret: [[0, 0, 5, 0, 5, 0.5, 15, 0.5, 15, 0, 20, 0]],
  callouts: [{ kind: 'separation', x: 10, y: 0.5, text: 'separation 0.50 mm, detour 1.00 mm' }, { kind: 'signal_pad', x: 0, y: 0, text: 'signal pad U1.A1' }], ...extra,
});

test('the signal is cyan, the return red, and the return carries arrow heads', () => {
  const { ctx, calls } = recorder();
  drawReturnPaths(ctx, [net('DDR_D0')], null, env);
  const strokes = calls.filter((c) => c.op === 'stroke');
  assert.equal(strokes[0].stroke, SIGNAL_COLOR);
  assert.equal(strokes[1].stroke, RETURN_COLOR);
  const heads = calls.filter((c) => c.op === 'fill' && c.fill === RETURN_COLOR);
  assert.ok(heads.length >= 3, `a 20 mm return (400 px) gets arrow heads, got ${heads.length}`);
  assert.equal(calls.filter((c) => c.op === 'fillText').length, 0); // callouts only for the selected net
});

test('the return is drawn from the end of the trace back to its start', () => {
  const { ctx, calls } = recorder();
  drawReturnPaths(ctx, [net('DDR_D0', { ret: [[0, 0, 20, 0]] })], null, env);
  // The second path drawn is the return: its first moveTo is the trace's last point (x = 20 -> 450 px).
  const moves = calls.filter((c) => c.op === 'moveTo');
  assert.equal(moves[0].args[0], 50); // the signal starts at its own start
  assert.ok(Math.abs((moves[1].args[0] as number) - 450) < 1e-9, `the return starts at the far end, got ${moves[1].args[0]}`);
});

test('the selected net gets callouts and a heavier line; the others are dimmed', () => {
  const { ctx, calls } = recorder();
  drawReturnPaths(ctx, [net('A'), net('B')], 'B', env);
  const texts = calls.filter((c) => c.op === 'fillText').map((c) => String(c.args[0]));
  assert.deepEqual(texts, ['separation 0.50 mm, detour 1.00 mm', 'signal pad U1.A1']);
  const dimmed = calls.filter((c) => c.op === 'stroke' && c.alpha < 1);
  const bright = calls.filter((c) => c.op === 'stroke' && c.alpha === 1);
  assert.ok(dimmed.length > 0 && bright.length > 0);
  assert.ok(Math.max(...bright.map((c) => c.width)) > Math.max(...dimmed.map((c) => c.width)));
});

test('a callout near the right edge is put to the left of its marker, clear of the side panel', () => {
  const { ctx, calls } = recorder();
  drawReturnPaths(ctx, [net('A', { callouts: [{ kind: 'separation', x: 40, y: 10, text: 'a fairly long callout text here' }] })], 'A', env);
  const box = calls.find((c) => c.op === 'fillRect')!;
  const [bx, , w] = box.args as number[];
  assert.ok(bx + w <= env.width - env.rightMargin, `box ends at ${bx + w}`);
  assert.ok(bx < 40 * 20 + 50); // on the marker's left
});

test('labels never overlap: a crowd of callouts keeps its markers and drops the text that has no room', () => {
  const { ctx, calls } = recorder();
  const crowd = Array.from({ length: 12 }, (_, i) => ({ kind: 'separation', x: 10 + i * 0.05, y: 5, text: `callout number ${i}` }));
  drawReturnPaths(ctx, [net('A', { callouts: crowd })], 'A', env);
  assert.equal(calls.filter((c) => c.op === 'arc').length, 12); // every marker
  const boxes = calls.filter((c) => c.op === 'fillRect').map((c) => c.args as number[]);
  assert.ok(boxes.length >= 2 && boxes.length < 12, `some text fits, not all: ${boxes.length}`);
  for (let i = 0; i < boxes.length; i++)
    for (let j = i + 1; j < boxes.length; j++) {
      const [x1, y1, w1, h1] = boxes[i], [x2, y2, w2, h2] = boxes[j];
      assert.ok(x1 + w1 <= x2 || x2 + w2 <= x1 || y1 + h1 <= y2 || y2 + h2 <= y1, `labels ${i} and ${j} overlap`);
    }
});

test('arrow heads point along the direction, and boxes cover every line', () => {
  const [tip, l, r] = arrowHead(10, 10, 1, 0, 8);
  assert.deepEqual(tip, [10, 10]);
  assert.ok(l[0] < 10 && r[0] < 10); // the wings are behind the tip
  assert.ok(l[1] * r[1] < 0 || (l[1] - 10) * (r[1] - 10) < 0); // on both sides of the line
  assert.deepEqual(pathBox(net('A')), [0, 0, 20, 0.5]);
  assert.equal(pathBox(net('B', { signal: [], ret: [] })), null);
});

test('an empty list draws nothing but the frame', () => {
  const { ctx, calls } = recorder();
  drawReturnPaths(ctx, [], null, env);
  assert.deepEqual(calls.map((c) => c.op), ['save', 'restore']);
});
