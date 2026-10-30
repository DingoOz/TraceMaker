// SPDX-License-Identifier: GPL-3.0-or-later
// Run: node --test src/burndown.test.ts
import test from 'node:test';
import assert from 'node:assert/strict';
import { Burndown, burndownSvg } from './burndown.ts';

test('points accumulate, a repeated time replaces the last point', () => {
  const b = new Burndown();
  b.add(0, 0, 10, 10);
  b.add(1, 4, 10, 6);
  b.add(1, 5, 10, 5);
  b.add(2, 9, 10, 1);
  assert.deepEqual(b.points.map((p) => [p.t, p.open]), [[0, 10], [1, 5], [2, 1]]);
  assert.equal(b.total, 10);
});

test('time going backwards or a new total starts a new run', () => {
  const b = new Burndown();
  b.add(5, 3, 10, 7);
  b.mark('widen');
  b.add(1, 0, 10, 10);
  assert.equal(b.points.length, 1);
  assert.equal(b.marks.length, 0);
  b.add(2, 1, 12, 11);
  assert.deepEqual(b.points, [{ t: 2, open: 11, routed: 1 }]);
  assert.equal(b.total, 12);
});

test('long runs are thinned but keep the first and latest points', () => {
  const b = new Burndown(100);
  for (let i = 0; i <= 1000; i++) b.add(i, i, 1000, 1000 - i);
  assert.ok(b.points.length <= 100);
  assert.equal(b.points[0].t, 0);
  assert.equal(b.points[b.points.length - 1].t, 1000);
  for (let i = 1; i < b.points.length; i++) assert.ok(b.points[i].t > b.points[i - 1].t);
});

test('stage marks: one per change, at the start before any data', () => {
  const b = new Burndown();
  b.mark('route');
  b.add(1, 0, 4, 4);
  b.mark('route');
  b.add(3, 2, 4, 2);
  b.mark('widen');
  assert.deepEqual(b.marks, [{ t: 0, name: 'route' }, { t: 3, name: 'widen' }]);
});

test('the chart steps from the total down and escapes stage names', () => {
  const b = new Burndown();
  b.add(0, 0, 100, 100);
  b.add(10, 50, 100, 50);
  b.add(20, 100, 100, 0);
  b.mark('a<b');
  const svg = burndownSvg(b, 200, 80);
  assert.match(svg, /^<svg [^>]*viewBox="0 0 200 80"/);
  assert.match(svg, /class="burn-line" d="M34,6H114V35H194V64"/);
  assert.match(svg, />100</);
  assert.match(svg, /a&lt;b at 20 s/);
  assert.doesNotMatch(svg, /a<b/);
});

test('an empty history draws axes only', () => {
  const svg = burndownSvg(new Burndown(), 200, 80);
  assert.doesNotMatch(svg, /burn-line/);
  assert.match(svg, /burn-axis/);
});

test('the server history replaces what the page had', () => {
  const b = new Burndown();
  b.add(9, 1, 5, 4);
  b.mark('late');
  b.load([[0, 0, 5, 5], [2, 3, 5, 2]], [[0, 'route']]);
  assert.deepEqual(b.points.map((p) => [p.t, p.open]), [[0, 5], [2, 2]]);
  assert.deepEqual(b.marks, [{ t: 0, name: 'route' }]);
  b.add(3, 4, 5, 1);
  assert.equal(b.points.length, 3);
});
