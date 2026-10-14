// SPDX-License-Identifier: GPL-3.0-or-later
// Run: node --test src/stall.test.ts (Node 22.6+ strips the types itself).
import test from 'node:test';
import assert from 'node:assert/strict';
import { StallTracker, stallLines, entryFrom, FREEZING_REASONS, PATHS_KEPT, DEFAULT_THRESHOLD, reasonColor } from './stall.ts';

const fail = (conn: number, extra: Record<string, unknown> = {}) => ({
  type: 'failure' as const, conn, net: conn + 1, rung: 1, cause: 'no legal path (pass 1)', a: [0, 0] as [number, number], b: [1000, 0] as [number, number],
  reason: 'no_legal_path', ...extra,
});
const stats = (routed: number, total = 10) => ({ type: 'stats' as const, stage: 'route', iteration: 1, routed, total, unrouted: total - routed, rips: 0, failures: 0, elapsed_s: 1 });
const track = (id: number) => ({ type: 'track_add' as const, track: { id, a: [0, 0] as [number, number], b: [1, 1] as [number, number], w: 100, layer: 0, net: 1 } });
const failures = (n: number, conn = 3) => Array.from({ length: n }, () => fail(conn));

test('a run of failures without progress latches; progress in between does not', () => {
  const t = new StallTracker();
  t.consume([stats(2)], true);
  t.consume(failures(DEFAULT_THRESHOLD - 1), true);
  assert.equal(t.latched, false);
  t.consume([stats(3)], true); // the routed count rose: the run starts over
  t.consume(failures(DEFAULT_THRESHOLD - 1), true);
  assert.equal(t.latched, false);
  t.consume(failures(1), true);
  assert.equal(t.latched, true);
  assert.equal(t.selected?.conn, 3);
  assert.equal(t.selected?.failures, DEFAULT_THRESHOLD);
});

test('budget expiry and the like do not freeze', () => {
  const t = new StallTracker();
  t.consume([stats(2)], true);
  t.consume(Array.from({ length: 100 }, () => fail(3, { reason: 'out_of_budget' })), true);
  t.consume(Array.from({ length: 100 }, () => fail(4, { reason: 'not_attempted' })), true);
  t.consume(Array.from({ length: 100 }, () => fail(5, { reason: undefined })), true); // an engine that sends no reason
  assert.equal(t.latched, false);
  assert.ok(!FREEZING_REASONS.has('out_of_budget'));
});

test('failures replayed while catching up do not freeze', () => {
  const t = new StallTracker();
  t.consume([stats(2)], false);
  t.consume(failures(200), false);
  assert.equal(t.latched, false);
});

test('a finished run is not a stall: the final stats in the same frame, or before the failures, keep the viewer unfrozen', () => {
  const done = { ...stats(2), stage: 'done' };
  const t = new StallTracker();
  t.consume([stats(2), ...failures(DEFAULT_THRESHOLD), done], true); // a late client: the history, the failures first
  assert.equal(t.latched, false);
  const t2 = new StallTracker();
  t2.consume([stats(2), done], true);
  t2.consume(failures(DEFAULT_THRESHOLD), true);
  assert.equal(t2.latched, false);
  // A run that goes on (a later stage) stalls as usual.
  const t3 = new StallTracker();
  t3.consume([stats(2)], true);
  t3.consume(failures(DEFAULT_THRESHOLD), true);
  assert.equal(t3.latched, true);
});

test('off: nothing is held back, whatever happens', () => {
  const t = new StallTracker();
  t.enabled = false;
  t.consume([stats(2)], true);
  const out = t.consume([...failures(200), track(1)], true);
  assert.equal(t.latched, false);
  assert.equal(out.length, 201);
  const board = { type: 'board' as const, name: 'x', bbox: [0, 0, 1, 1] as [number, number, number, number], layers: [], nets: [], outline: [], pads: [], tracks: [], vias: [], zones: [], footprints: [], freeze: false };
  const t2 = new StallTracker();
  t2.consume([board], true);
  assert.equal(t2.enabled, false);
});

test('while latched the scene is held, the HUD is not, and dismissal gives back everything in order', () => {
  const t = new StallTracker();
  t.consume([stats(2), ...failures(DEFAULT_THRESHOLD)], true);
  assert.equal(t.latched, true);
  const during = [track(1), { type: 'frontier' as const, conn: 3, layer: 0, pts: [[0, 0] as [number, number]] }, stats(2), track(2), { type: 'log' as const, level: 'info', text: 'x' }, track(3)];
  const applied = t.consume(during, true);
  assert.deepEqual(applied.map((m) => m.type), ['stats', 'log']); // HUD messages pass; copper does not; the frontier is dropped
  const back = t.dismiss();
  assert.deepEqual(back.map((m) => (m.type === 'track_add' ? m.track.id : m.type)), [1, 2, 3]);
  assert.equal(t.latched, false);
  assert.deepEqual(t.dismiss(), []); // nothing more to give
});

test('after dismissal the next stall needs a fresh run of failures (it re-arms)', () => {
  const t = new StallTracker();
  t.consume([stats(2), ...failures(DEFAULT_THRESHOLD)], true);
  t.dismiss();
  t.consume(failures(DEFAULT_THRESHOLD - 1), true);
  assert.equal(t.latched, false);
  t.consume(failures(1), true);
  assert.equal(t.latched, true);
});

test('[ and ] move through the stalled connections; a click picks one by net', () => {
  const t = new StallTracker();
  t.threshold = 6;
  t.consume([stats(2)], true);
  t.consume([fail(10), fail(11), fail(12), fail(10), fail(11), fail(12)], true);
  assert.equal(t.latched, true);
  assert.deepEqual(t.entries.map((e) => e.conn), [10, 11, 12]);
  assert.equal(t.selected?.conn, 12);
  assert.equal(t.select(1)?.conn, 10); // wraps
  assert.equal(t.select(-1)?.conn, 12);
  assert.equal(t.select(-1)?.conn, 11);
  assert.equal(t.selectNet(11), true); // conn 10 is net 11
  assert.equal(t.selected?.conn, 10);
  assert.equal(t.selectNet(999), false);
  assert.equal(t.selectIndex(2)?.conn, 12);
  assert.equal(t.selectIndex(3), null);
  assert.equal(t.selectIndex(-1), null);
});

test('rejected paths are kept per connection, the last PATHS_KEPT of them, newest last', () => {
  let e = undefined as ReturnType<typeof entryFrom> | undefined;
  for (let i = 0; i < PATHS_KEPT + 5; i++) e = entryFrom(fail(3, { rejected: [[i, 0, 0], [i, 5, 0]] }), e);
  assert.equal(e!.paths.length, PATHS_KEPT);
  assert.equal(e!.paths[PATHS_KEPT - 1][0][0], PATHS_KEPT + 4); // the newest
  assert.equal(e!.paths[0][0][0], 5); // the first five were dropped
  assert.equal(e!.failures, PATHS_KEPT + 5);
  // A path of one point is no path (the engine leaves such a path out; a reader that is sent one ignores it).
  assert.equal(entryFrom(fail(3, { rejected: [[1, 1, 0]] }), undefined).paths.length, 0);
});

test('a snapshot starts everything over', () => {
  const t = new StallTracker();
  t.consume([stats(2), ...failures(DEFAULT_THRESHOLD)], true);
  assert.equal(t.latched, true);
  const board = { type: 'board' as const, name: 'x', bbox: [0, 0, 1, 1] as [number, number, number, number], layers: [], nets: [], outline: [], pads: [], tracks: [], vias: [], zones: [], footprints: [] };
  const out = t.consume([board], true);
  assert.equal(out.length, 1);
  assert.equal(t.latched, false);
});

test('an old engine: failures without any of the new fields still work, and say they have no measurement', () => {
  const t = new StallTracker();
  const old = { type: 'failure' as const, conn: 1, net: 2, rung: 1, cause: 'no legal path (pass 1)', a: [0, 0] as [number, number], b: [5, 5] as [number, number], blockers: [] };
  const e = entryFrom(old, undefined);
  assert.equal(e.verdict, 'unknown');
  assert.equal(e.widthNm, 0);
  assert.match(stallLines(e, ['F.Cu', 'B.Cu']).label, /no measurement/);
  t.consume([stats(0), old], true); // does not throw, does not latch
  assert.equal(t.latched, false);
});

test('the label carries the engine numbers: width, clearance, the widest track that fits, the layer, and the verdict', () => {
  const e = entryFrom(fail(1, { width_nm: 250000, clearance_nm: 200000, min_width_nm: 100000, gap_nm: 120000, gap_fixed_nm: 120000, gap_layer: 1, verdict: 'rule', rule: 'netclass Power: width 0.25 mm' }), undefined);
  const l = stallLines(e, ['F.Cu', 'B.Cu']);
  assert.equal(l.label, 'need 0.25 mm track + 0.2 mm clearance; widest track that fits 0.12 mm on B.Cu');
  assert.match(l.verdict, /^rule:/);
  assert.equal(l.rule, 'netclass Power: width 0.25 mm');
  const c = stallLines(entryFrom(fail(1, { width_nm: 250000, clearance_nm: 100000, gap_nm: 0, gap_fixed_nm: 250000, gap_layer: 0, verdict: 'contested' }), undefined), ['F.Cu']);
  assert.match(c.label, /widest track that fits 0 mm on F\.Cu \(0\.25 mm with only fixed copper\)/);
  assert.match(c.verdict, /^contested:/);
  assert.match(stallLines(entryFrom(fail(1, { verdict: 'nonsense' }), undefined), []).verdict, /^unknown:/); // an unknown verdict word is unknown
});

test('colours differ by cause', () => {
  const cs = new Set(['boxed_in', 'no_legal_path', 'exact_check', 'rip_limit'].map(reasonColor));
  assert.equal(cs.size, 4);
});
