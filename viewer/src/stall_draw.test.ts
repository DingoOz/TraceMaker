// SPDX-License-Identifier: GPL-3.0-or-later
// Run: node --test src/stall_draw.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { drawStall, placeLabel, type Canvas2D } from './stall_draw.ts';
import { entryFrom } from './stall.ts';

// A context that records what is drawn.
function recorder() {
  const calls: { op: string; args: unknown[]; style?: string }[] = [];
  const ctx: Record<string, unknown> = { strokeStyle: '', fillStyle: '', lineWidth: 1, font: '', textAlign: 'left', textBaseline: 'alphabetic', globalAlpha: 1 };
  for (const op of ['save', 'restore', 'beginPath', 'moveTo', 'lineTo', 'arc', 'stroke', 'fill', 'fillRect', 'strokeRect', 'fillText', 'setLineDash'])
    ctx[op] = (...args: unknown[]) => calls.push({ op, args, style: String(ctx.strokeStyle) });
  ctx.measureText = (t: string) => ({ width: t.length * 6.5 });
  return { ctx: ctx as unknown as Canvas2D, calls };
}
const env = { toScreen: (x: number, y: number): [number, number] => [x / 1e4 + 100, y / 1e4 + 100], width: 1200, height: 800, activeLayer: 0, layerNames: ['F.Cu', 'B.Cu'], rightMargin: 340 };
const fail = (extra: Record<string, unknown>) => ({
  type: 'failure' as const, conn: 1, net: 2, rung: 1, cause: 'no legal path', a: [0, 0] as [number, number], b: [800000, 0] as [number, number], reason: 'boxed_in',
  width_nm: 250000, clearance_nm: 200000, gap_nm: 0, gap_fixed_nm: 0, gap_layer: 0, verdict: 'placement' as const, rule: 'netclass Default: width 0.25 mm', ...extra,
});

test('draws each rejected path as a polyline, the two ends, the mark, and the label with the verdict', () => {
  const e = entryFrom(fail({ rejected: [[0, 0, 0], [300000, 0, 0], [300000, 100000, 0]], at: [300000, 100000] }), undefined);
  const e2 = entryFrom(fail({ rejected: [[0, 0, 0], [350000, 0, 0]], at: [350000, 0] }), e);
  const { ctx, calls } = recorder();
  drawStall(ctx, e2, env);
  const polylines = calls.filter((c) => c.op === 'moveTo').length;
  assert.ok(polylines >= 2 + 2, 'two paths, and the two strokes of the cross'); // 2 path starts + 2 cross strokes
  assert.equal(calls.filter((c) => c.op === 'arc').length, 2); // rings at both pads
  const texts = calls.filter((c) => c.op === 'fillText').map((c) => String(c.args[0]));
  assert.equal(texts.length, 3);
  assert.match(texts[0], /need 0\.25 mm track \+ 0\.2 mm clearance; widest track that fits 0 mm on F\.Cu/);
  assert.match(texts[1], /^placement:/);
  assert.match(texts[2], /^netclass Default/);
  assert.equal(calls[0].op, 'save');
  assert.equal(calls[calls.length - 1].op, 'restore');
});

test('the path is drawn in the colour of its cause', () => {
  const draw = (reason: string) => {
    const { ctx, calls } = recorder();
    drawStall(ctx, entryFrom(fail({ reason, rejected: [[0, 0, 0], [1000, 0, 0]] }), undefined), env);
    return calls.find((c) => c.op === 'stroke')!.style;
  };
  assert.notEqual(draw('boxed_in'), draw('no_legal_path'));
  assert.notEqual(draw('no_legal_path'), draw('exact_check'));
});

test('no measurement and no path still draws the ends and says so', () => {
  const { ctx, calls } = recorder();
  const e = entryFrom({ type: 'failure', conn: 1, net: 2, rung: 1, cause: 'no legal path', a: [0, 0], b: [1000, 1000] }, undefined);
  drawStall(ctx, e, env);
  assert.equal(calls.filter((c) => c.op === 'arc').length, 2);
  assert.match(String(calls.filter((c) => c.op === 'fillText')[0].args[0]), /no measurement/);
});

test('the label stays on the screen and clear of the side panel', () => {
  const box = (ax: number, ay: number) => placeLabel(ax, ay, 400, 70, env);
  for (const [ax, ay] of [[10, 10], [1190, 790], [860, 400], [600, -50], [600, 900]]) {
    const [x, y] = box(ax, ay);
    assert.ok(x >= 8 && x + 400 <= env.width - env.rightMargin - 8 + 1e-9, `x ${x} for anchor ${ax}`);
    assert.ok(y >= 8 && y + 70 <= env.height, `y ${y} for anchor ${ay}`);
  }
  assert.ok(box(300, 300)[0] > 300); // room on the right: it goes there
  assert.ok(box(800, 300)[0] < 800); // no room: it goes to the left
  // It keeps off the line through the anchor, where the rejected path arrives: above it when there is room, else below.
  assert.ok(box(300, 300)[1] + 70 < 300);
  assert.ok(box(300, 40)[1] > 40);
  for (const [ax, ay] of [[300, 300], [300, 40], [800, 300], [100, 760]]) {
    const [x, y] = box(ax, ay);
    const inside = ax >= x && ax <= x + 400 && ay >= y && ay <= y + 70;
    assert.ok(!inside, `the anchor ${ax},${ay} is under the label`);
  }
});
