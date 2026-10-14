// SPDX-License-Identifier: GPL-3.0-or-later
// Run: node --experimental-strip-types --test src/footprintcheck_draw.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { drawFootprintCheck, ERROR_COLOR, findingBox, findingColor, WARNING_COLOR } from './footprintcheck_draw.ts';
import type { Canvas2D } from './stall_draw.ts';
import type { FootprintFinding } from './scene.ts';

function recorder() {
  const calls: { op: string; args: unknown[]; stroke: string; fill: string; alpha: number }[] = [];
  const ctx: Record<string, unknown> = { strokeStyle: '', fillStyle: '', lineWidth: 1, font: '', textAlign: 'left', textBaseline: 'alphabetic', globalAlpha: 1, lineCap: 'butt', lineJoin: 'miter' };
  for (const op of ['save', 'restore', 'beginPath', 'moveTo', 'lineTo', 'arc', 'stroke', 'fill', 'fillRect', 'strokeRect', 'fillText', 'setLineDash'])
    ctx[op] = (...args: unknown[]) => calls.push({ op, args, stroke: String(ctx.strokeStyle), fill: String(ctx.fillStyle), alpha: Number(ctx.globalAlpha) });
  ctx.measureText = (t: string) => ({ width: t.length * 6.5 });
  return { ctx: ctx as unknown as Canvas2D, calls };
}
const env = { toScreen: (x: number, y: number): [number, number] => [x * 20 + 50, y * 20 + 50], width: 1200, height: 800, rightMargin: 340 };

const finding = (ref: string, severity: string, extra: Partial<FootprintFinding> = {}): FootprintFinding => ({
  ref, libId: 'Package_SO:SOIC-8', status: severity === 'error' ? 'mismatch' : 'untrusted', x: 10, y: 10, datasheet: 'Example Semiconductor EX1234 (EX1234 datasheet rev C, p.21)',
  issues: [{ severity, kind: 'position', pad: '3', text: 'pad 3 is 0.15 mm from where the datasheet land pattern puts it', label: 'pad 3: 0.15 mm off', x: 10.5, y: 10.2 }], ...extra,
});

test('a footprint with an error gets a red ring, one with only warnings an amber one', () => {
  assert.equal(findingColor(finding('U1', 'error')), ERROR_COLOR);
  assert.equal(findingColor(finding('U2', 'warning')), WARNING_COLOR);
  const { ctx, calls } = recorder();
  drawFootprintCheck(ctx, [finding('U1', 'error'), finding('U2', 'warning', { x: 20 })], null, env);
  const arcs = calls.filter((c) => c.op === 'arc');
  assert.equal(arcs.length, 2);
  const strokeAfterArc = (i: number) => calls.slice(calls.indexOf(arcs[i])).find((c) => c.op === 'stroke')!;
  assert.equal(strokeAfterArc(0).stroke, ERROR_COLOR);
  assert.equal(strokeAfterArc(1).stroke, WARNING_COLOR);
  assert.equal(calls.filter((c) => c.op === 'fillText').length, 0); // messages only for the selected footprint
});

test('the selected footprint gets a mark and the message at each issue; the others are dimmed', () => {
  const { ctx, calls } = recorder();
  drawFootprintCheck(ctx, [finding('U1', 'error'), finding('U2', 'error', { x: 30 })], 'U2', env);
  const texts = calls.filter((c) => c.op === 'fillText').map((c) => String(c.args[0]));
  assert.equal(texts.length, 1);
  assert.equal(texts[0], 'pad 3: 0.15 mm off');
  const rings = calls.filter((c) => c.op === 'arc');
  assert.equal(rings[0].alpha, 0.5); // U1, not selected
  assert.equal(rings[1].alpha, 1);
  assert.ok((rings[1].args[2] as number) > (rings[0].args[2] as number)); // the selected ring is larger
});

test('labels that would cover each other are left out, the marks stay', () => {
  const issues = Array.from({ length: 12 }, (_, k) => ({ severity: 'error', kind: 'position', pad: String(k), text: `pad ${k} is off by a good deal of distance`, label: `pad ${k}: 0.2 mm off`, x: 10 + 0.01 * k, y: 10 }));
  const { ctx, calls } = recorder();
  drawFootprintCheck(ctx, [finding('U1', 'error', { issues })], 'U1', env);
  const crosses = calls.filter((c) => c.op === 'moveTo').length / 2;
  const labels = calls.filter((c) => c.op === 'fillText').length;
  assert.equal(crosses, 12);
  assert.ok(labels >= 1 && labels < 12, `labels ${labels}`);
});

test('a long message is cut, and nothing is drawn off screen', () => {
  const { ctx, calls } = recorder();
  const long = finding('U1', 'error', { issues: [{ severity: 'error', kind: 'pattern', pad: '', text: 'x'.repeat(300), label: 'x'.repeat(300), x: 2, y: 10 }] });
  drawFootprintCheck(ctx, [long, finding('U9', 'error', { x: -500, y: -500 })], 'U1', env);
  const text = String(calls.find((c) => c.op === 'fillText')!.args[0]);
  assert.ok(text.length <= 60 && text.endsWith('…'));
  assert.equal(calls.filter((c) => c.op === 'arc').length, 1); // U9 is off screen
});

test('the box of a finding covers the footprint and its marks', () => {
  assert.deepEqual(findingBox(finding('U1', 'error')), [10, 10, 10.5, 10.2]);
});
