// SPDX-License-Identifier: GPL-3.0-or-later
// Freeze on a routing stall (issue #8, docs/09-visualization.md section 8, docs/13-viewer-protocol.md "The failure verdict").
//
// A stall is: the routed count has not gone up for `threshold` failed attempts (a count of the engine's own work, not a clock), and the
// latest failure is one a rule or a placement could explain (not a budget that ran out). The viewer then latches: it holds the scene on
// the connection that stopped, with the rejected paths and the numbers beside them, until the user dismisses it. The engine does not
// pause; the messages that arrive meanwhile are kept in order and applied on dismissal, so nothing is lost and nothing is reordered.
//
// This module is plain data and logic (no DOM, no WebGL) so it can be tested under Node: stall.test.ts.
import type { FailureMsg, Message, XY } from './protocol';

export type Verdict = 'placement' | 'rule' | 'contested' | 'unknown';

/** What one stalled connection shows: its latest failure and the paths that were rejected since the count last moved. */
export interface StallEntry {
  conn: number;
  net: number;
  reason: string; // boxed_in, no_legal_path, exact_check, ...
  cause: string;
  verdict: Verdict;
  rung: number;
  a: XY;
  b: XY;
  at: XY | null;
  widthNm: number; // the attempt's track width, 0 = not measured
  clearanceNm: number;
  minWidthNm: number;
  gapNm: number; // widest track that gets through with routed copper counted, -1 = not measured
  gapFixedNm: number; // with only fixed copper counted
  gapLayer: number;
  rule: string;
  paths: [number, number, number][][]; // rejected paths (nm, layer), the last PATHS_KEPT
  failures: number; // how many failed attempts of this connection since the count last moved
}

/** Reasons that can end a stall that is worth looking at. Budget expiry, a skipped identical attempt, a rolled-back transaction and the
 *  like are not: they say nothing about a gap. */
export const FREEZING_REASONS = new Set(['boxed_in', 'no_legal_path', 'exact_check', 'rip_limit', 'layer_limit']);
/** Rejected paths kept per connection (docs/13: 16 is enough). */
export const PATHS_KEPT = 16;
/** Entries kept, oldest dropped. */
export const ENTRIES_KEPT = 24;
/** Failed attempts without the routed count rising that make a stall. */
export const DEFAULT_THRESHOLD = 24;
/** Messages held while latched; past this the viewer cannot resync by itself and asks for a reload. */
export const BUFFER_LIMIT = 400_000;

const STATE_MESSAGES = new Set(['track_add', 'track_remove', 'via_add', 'via_remove', 'footprint_move', 'ratsnest', 'heatmap', 'escape_plan', 'escape_release', 'escape_dead', 'failure', 'board']);
const TRANSIENT_MESSAGES = new Set(['frontier', 'path_try']);

function verdictOf(v: string | undefined): Verdict {
  return v === 'placement' || v === 'rule' || v === 'contested' ? v : 'unknown';
}

export function entryFrom(m: FailureMsg, previous: StallEntry | undefined): StallEntry {
  const paths = previous ? previous.paths.slice() : [];
  if (m.rejected && m.rejected.length >= 2) paths.push(m.rejected);
  while (paths.length > PATHS_KEPT) paths.shift();
  return {
    conn: m.conn,
    net: m.net,
    reason: m.reason ?? '',
    cause: m.cause,
    verdict: verdictOf(m.verdict),
    rung: m.rung,
    a: m.a,
    b: m.b,
    at: m.at ?? null,
    widthNm: m.width_nm ?? 0,
    clearanceNm: m.clearance_nm ?? 0,
    minWidthNm: m.min_width_nm ?? 0,
    gapNm: m.gap_nm ?? -1,
    gapFixedNm: m.gap_fixed_nm ?? -1,
    gapLayer: m.gap_layer ?? -1,
    rule: m.rule ?? '',
    paths,
    failures: (previous?.failures ?? 0) + 1,
  };
}

export class StallTracker {
  /** Off (`#freeze=0`, or the engine said `freeze: false` in the board snapshot): failures are still collected, nothing is held back. */
  enabled = true;
  threshold = DEFAULT_THRESHOLD;
  /** The buffer overflowed while latched: dismissing cannot restore the scene, the page has to reload. */
  overflowed = false;

  private routed = -1;
  private finished = false; // the engine said it is done: whatever failed is history, not a stall in progress
  private failuresSince = 0;
  private recent = new Map<number, StallEntry>(); // by connection, in order of the latest failure
  private held: Message[] = [];
  private _latched = false;
  private order: number[] = [];
  private index = 0;

  get latched(): boolean { return this._latched; }
  /** The connections of the stall, oldest first. */
  get entries(): StallEntry[] { return this.order.map((c) => this.recent.get(c)!).filter(Boolean); }
  get selected(): StallEntry | null { return this._latched ? this.entries[this.index] ?? null : null; }
  get selectedIndex(): number { return this.index; }
  get held_count(): number { return this.held.length; }

  /**
   * Takes the messages of one animation frame and returns the ones to apply to the scene now. `live` is false while the viewer catches up
   * after a snapshot (old failures replayed from the server's history must not freeze it).
   */
  consume(msgs: Message[], live: boolean): Message[] {
    const out: Message[] = [];
    // A frame that carries the final stats ("done") is a run that has ended (a viewer that joins late is sent the history, failures
    // first): a clean finish and a budget that ran out are not stalls.
    const batchDone = msgs.some((m) => m.type === 'stats' && m.stage === 'done');
    if (batchDone) this.finished = true;
    for (const m of msgs) {
      if (m.type === 'board') {
        // A new snapshot starts everything over (a reconnect, or another board).
        this.reset();
        if (m.freeze === false) this.enabled = false;
        out.push(m);
        continue;
      }
      if (this._latched) {
        if (TRANSIENT_MESSAGES.has(m.type)) continue; // the search frontier of a latched scene is not worth keeping
        if (m.type === 'stats' || m.type === 'stage' || m.type === 'log') {
          out.push(m); // the HUD keeps counting
          continue;
        }
        if (STATE_MESSAGES.has(m.type)) {
          if (this.held.length < BUFFER_LIMIT) this.held.push(m);
          else this.overflowed = true;
          // A failure that arrives while latched still belongs to the picture the user is looking at.
          if (m.type === 'failure') this.note(m, false);
          continue;
        }
        out.push(m);
        continue;
      }
      if (m.type === 'stats') {
        if (m.routed > this.routed && this.routed >= 0) this.progress();
        this.routed = m.routed;
        this.finished = batchDone || m.stage === 'done';
      } else if (m.type === 'failure') {
        this.note(m, live);
      }
      out.push(m);
    }
    return out;
  }

  /** Clears the latch and returns what was held, in the order it arrived. */
  dismiss(): Message[] {
    if (!this._latched) return [];
    this._latched = false;
    const h = this.held;
    this.held = [];
    // Re-armed: the next stall needs a fresh run of failures.
    this.failuresSince = 0;
    this.recent.clear();
    this.order = [];
    this.index = 0;
    return h;
  }

  /** `[` and `]`: the previous or next stalled connection. */
  select(delta: number): StallEntry | null {
    if (!this._latched || !this.order.length) return null;
    this.index = (this.index + delta + this.order.length) % this.order.length;
    return this.selected;
  }

  /** A click on a row of the stall panel. */
  selectIndex(i: number): StallEntry | null {
    if (!this._latched || i < 0 || i >= this.order.length) return null;
    this.index = i;
    return this.selected;
  }

  /** Picks a connection by net (a click on a net's ratsnest edge or track): true if it is one of the stalled ones. */
  selectNet(net: number): boolean {
    if (!this._latched) return false;
    const i = this.order.findIndex((c) => this.recent.get(c)?.net === net);
    if (i < 0) return false;
    this.index = i;
    return true;
  }

  private reset() {
    this.routed = -1;
    this.finished = false;
    this.failuresSince = 0;
    this.recent.clear();
    this.order = [];
    this.held = [];
    this._latched = false;
    this.index = 0;
    this.overflowed = false;
  }

  private progress() {
    this.failuresSince = 0;
    this.recent.clear();
  }

  private note(m: FailureMsg, live: boolean) {
    const prev = this.recent.get(m.conn);
    this.recent.delete(m.conn); // re-inserted last: the map is in order of the latest failure
    this.recent.set(m.conn, entryFrom(m, prev));
    while (this.recent.size > ENTRIES_KEPT) this.recent.delete(this.recent.keys().next().value as number);
    if (this._latched) {
      if (!this.order.includes(m.conn)) this.order.push(m.conn);
      return;
    }
    ++this.failuresSince;
    if (!this.enabled || !live || this.finished || this.failuresSince < this.threshold) return;
    if (!FREEZING_REASONS.has(m.reason ?? '')) return;
    // Latch on the connection that just failed; the others that failed since the count moved are one key press away.
    this._latched = true;
    this.order = [...this.recent.keys()].filter((c) => FREEZING_REASONS.has(this.recent.get(c)!.reason));
    this.index = Math.max(0, this.order.indexOf(m.conn));
  }
}

// ----------------------------------------------------------------------------------------------- text

const mm = (nm: number) => {
  const v = nm / 1e6;
  return `${v >= 1 ? v.toFixed(2) : v.toFixed(3).replace(/0+$/, '').replace(/\.$/, '')} mm`;
};

export const VERDICT_TEXT: Record<Verdict, string> = {
  placement: 'placement: no legal track fits; moving a part is the next step',
  rule: 'rule: a narrower legal track or the board minimum clearance fits; change the net class or the rule',
  contested: 'contested: fixed copper leaves room, routed copper of other nets closes it; that is negotiation',
  unknown: 'unknown: not enough evidence to send you to a footprint or a rule',
};

/** The label beside the closest approach and the verdict under it (issue #8): the numbers are the engine's, not measured again here. */
export function stallLines(e: StallEntry, layerNames: string[]): { label: string; verdict: string; rule: string } {
  let label: string;
  if (e.widthNm > 0) {
    const layer = e.gapLayer >= 0 ? ` on ${layerNames[e.gapLayer] ?? `layer ${e.gapLayer}`}` : '';
    const fits = e.gapNm >= 0 ? `widest track that fits ${mm(e.gapNm)}${layer}` : 'gap not measured';
    const fixed = e.verdict === 'contested' && e.gapFixedNm >= 0 ? ` (${mm(e.gapFixedNm)} with only fixed copper)` : '';
    label = `need ${mm(e.widthNm)} track + ${mm(e.clearanceNm)} clearance; ${fits}${fixed}`;
  } else {
    label = `no measurement (${e.reason || e.cause})`;
  }
  return { label, verdict: VERDICT_TEXT[e.verdict], rule: e.rule };
}

/** Colours (CSS) by cause, not one red for all (issue #8). */
export function reasonColor(reason: string): string {
  switch (reason) {
    case 'boxed_in': return '#ff6ad5'; // the pin cannot get out
    case 'no_legal_path': return '#ff4d5e'; // blocked
    case 'exact_check': return '#ffb454'; // the lattice path failed the exact check
    case 'rip_limit': return '#a78bfa'; // only ripping protected copper would do
    default: return '#9aa7c7';
  }
}

export function verdictColor(v: Verdict): string {
  return v === 'placement' ? '#ff5d6c' : v === 'rule' ? '#5b9cf5' : v === 'contested' ? '#f2b84b' : '#9aa7c7';
}
