// SPDX-License-Identifier: GPL-3.0-or-later
// HTML overlay: brand bar, progress, layer controls, log, status bar and hover tooltip.
import { css, infernoGradient, layerColor } from './colors';
import type { ConnState } from './connection';
import type { StatsMsg } from './protocol';
import { verdictColor, type Verdict } from './stall';

const h = <K extends keyof HTMLElementTagNameMap>(tag: K, cls?: string, text?: string): HTMLElementTagNameMap[K] => {
  const e = document.createElement(tag);
  if (cls) e.className = cls;
  if (text !== undefined) e.textContent = text;
  return e;
};

const fmt = new Intl.NumberFormat('en-US');
export const num = (v: number) => fmt.format(Math.round(v));

export function duration(s: number): string {
  if (!isFinite(s) || s < 0) return '–';
  const t = Math.floor(s);
  const hh = Math.floor(t / 3600), mm = Math.floor((t % 3600) / 60), ss = t % 60;
  const p = (v: number) => String(v).padStart(2, '0');
  return hh > 0 ? `${hh}:${p(mm)}:${p(ss)}` : `${p(mm)}:${p(ss)}`;
}

/** What the stall panel shows (issue #8): one row per stalled connection, the selected one in full. */
export interface StallView {
  rows: { net: string; reason: string; verdict: Verdict; failures: number }[];
  index: number;
  label: string;
  verdict: string;
  rule: string;
}

export interface Toggles {
  activeOnTop: boolean;
  zones: boolean;
  ratsnest: boolean;
  escape: boolean;
  footprints: boolean;
  effects: boolean;
  follow: boolean;
  returnPaths: boolean;
  footprintCheck: boolean;
}

const LOGO = `<svg viewBox="0 0 64 64" width="24" height="24" aria-hidden="true"><rect width="64" height="64" rx="14" fill="#101a2e"/><g fill="none" stroke-linejoin="round"><polyline points="15,42 17,42 27,32 28.2,32" stroke="#ef5b5b" stroke-width="5"/><polyline points="35.8,32 37,32 47,42 49,42" stroke="#5b9cf5" stroke-width="5"/><circle cx="11" cy="42" r="4.5" stroke="#ef5b5b" stroke-width="3.2"/><circle cx="53" cy="42" r="4.5" stroke="#5b9cf5" stroke-width="3.2"/><circle cx="32" cy="32" r="3.8" stroke="#e8ecf2" stroke-width="3.4"/></g></svg>`;

export class Hud {
  readonly root: HTMLElement;
  private boardName = h('span', 'board-name', '—');
  private status = h('span', 'status connecting', 'Connecting');
  private stageName = h('div', 'stage-name', 'Waiting for engine');
  private stageDetail = h('div', 'stage-detail', '');
  private iteration = h('span', 'mono', '–');
  private routedText = h('span', 'mono', '– / –');
  private pct = h('span', 'pct mono', '');
  private bar = h('div', 'bar-fill');
  private cells = new Map<string, HTMLElement>();
  private extra = h('div', 'extra');
  private layerList = h('div', 'layer-list');
  private chips = h('div', 'chips');
  private heat = h('div', 'heat');
  private log = h('div', 'log-list');
  private logCount = 0;
  private coords = h('span', 'mono', '');
  private zoom = h('span', 'mono', '');
  readonly tooltip = h('div', 'tooltip');
  private rpBox = h('section', 'section hidden');
  private fcBox = h('section', 'section hidden');
  private cnBox = h('section', 'section hidden');
  private stallBox = h('section', 'section stall hidden');
  private banner = h('div', 'panel stall-banner hidden');
  private fatalBox: HTMLElement | null = null;

  onLayerVisible: (layer: number, visible: boolean) => void = () => {};
  onActiveLayer: (layer: number) => void = () => {};
  onToggle: (key: keyof Toggles, value: boolean) => void = () => {};
  onFit: () => void = () => {};
  onHeatmap: (name: string) => void = () => {};
  onStallDismiss: () => void = () => {};
  onStallPick: (index: number) => void = () => {};
  onReturnPath: (name: string | null) => void = () => {};
  onFootprintCheck: (ref: string | null) => void = () => {};
  onCriticalNet: (name: string) => void = () => {};

  constructor(parent: HTMLElement) {
    this.root = h('div', 'hud');
    parent.appendChild(this.root);

    // Brand bar.
    const brand = h('header', 'panel brand');
    const logo = h('span', 'logo');
    logo.innerHTML = LOGO;
    const title = h('div', 'title');
    title.append(h('span', 'product', 'TraceMaker'), h('span', 'sep', '/'), this.boardName);
    brand.append(logo, title, this.status);
    this.root.appendChild(brand);

    // Side panel.
    const side = h('aside', 'panel side');
    const prog = h('section', 'section');
    const head = h('div', 'stage-head');
    const stageBox = h('div', 'stage-box');
    stageBox.append(h('div', 'label', 'Stage'), this.stageName, this.stageDetail);
    const iterBox = h('div', 'iter-box');
    iterBox.append(h('div', 'label', 'Iteration'), this.iteration);
    head.append(stageBox, iterBox);
    const progRow = h('div', 'prog-row');
    const routedLabel = h('span', 'prog-label');
    routedLabel.append(this.routedText, h('span', 'muted', ' routed'));
    progRow.append(routedLabel, this.pct);
    const bar = h('div', 'bar');
    bar.appendChild(this.bar);
    const grid = h('div', 'grid');
    for (const [key, label] of [['unrouted', 'Unrouted'], ['rips', 'Rip-ups'], ['failures', 'Failures'], ['elapsed', 'Elapsed'], ['rate', 'Msg / s'], ['fps', 'FPS']] as const) {
      const cell = h('div', 'cell');
      const v = h('div', 'value mono', '–');
      if (key === 'failures') v.classList.add('bad');
      cell.append(h('div', 'label', label), v);
      this.cells.set(key, v);
      grid.appendChild(cell);
    }
    prog.append(head, progRow, bar, grid, this.extra);

    const layers = h('section', 'section');
    const lh = h('div', 'section-head');
    lh.append(h('div', 'label', 'Layers'), h('div', 'hint', 'click = active · 1–9'));
    layers.append(lh, this.layerList, this.chips, this.heat);
    this.setHeatmap('', [], 0, '');

    const logSec = h('section', 'section grow');
    const logHead = h('div', 'section-head');
    const clear = h('button', 'link', 'Clear');
    clear.onclick = () => { this.log.textContent = ''; this.logCount = 0; };
    logHead.append(h('div', 'label', 'Events'), clear);
    logSec.append(logHead, this.log);

    side.append(this.stallBox, prog, this.cnBox, this.fcBox, this.rpBox, layers, logSec);
    this.root.appendChild(side);

    // Status bar.
    const sb = h('footer', 'panel statusbar');
    const fit = h('button', 'btn', 'Fit');
    fit.title = 'Fit board (F)';
    fit.onclick = () => this.onFit();
    sb.append(fit, this.coords, this.zoom, h('span', 'hint', 'drag to pan · wheel to zoom · F fit · H high contrast · M heatmap'));
    this.root.appendChild(sb);
    this.root.appendChild(this.tooltip);
    this.root.appendChild(this.banner);
  }

  /** The return-path list (issue #7): one row per audited net, worst detour first; a click selects it (and again deselects). */
  setReturnPaths(rows: { name: string; detourMm: number; maxSeparationMm: number; voids: number; splits: number; missingStitches: number }[], selected: string | null) {
    this.rpBox.textContent = '';
    this.rpBox.classList.toggle('hidden', rows.length === 0);
    if (!rows.length) return;
    const head = h('div', 'section-head');
    head.append(h('div', 'label', 'Return paths'), h('div', 'hint', 'cyan signal · red return · P toggles'));
    this.rpBox.appendChild(head);
    const list = h('div', 'stall-list');
    for (const r of rows) {
      const bad = r.splits > 0 || r.missingStitches > 0 || r.detourMm > 0;
      const row = h('div', `stall-row${r.name === selected ? ' on' : ''}`);
      const chip = h('span', 'chip-verdict', r.splits ? 'split' : r.detourMm > 0 ? `+${r.detourMm.toFixed(1)}` : r.missingStitches ? 'stitch' : 'ok');
      chip.style.background = r.splits ? '#ff5d6c' : bad ? '#f2b84b' : '#5fd38d';
      row.append(chip, h('span', 'stall-net', r.name), h('span', 'muted mono', `${r.maxSeparationMm.toFixed(2)} mm`));
      row.title = `${r.voids} void(s), detour ${r.detourMm.toFixed(2)} mm, farthest ${r.maxSeparationMm.toFixed(2)} mm, ${r.missingStitches} missing stitch(es)`;
      row.onclick = () => this.onReturnPath(r.name === selected ? null : r.name);
      list.appendChild(row);
    }
    this.rpBox.appendChild(list);
  }

  /** The critical nets after a route (issue #24): tier chip, name, vias; amber when a rule was missed (a split crossed, vias over the limit). A click on a net
   *  whose return path was sent shows it. */
  setCriticalNets(
    v: { summary: string; rows: { name: string; tier: number; why: string; vias: number; crossings: number; unrouted: number; warnings: string[] }[] } | null,
    selected: string | null,
  ) {
    this.cnBox.textContent = '';
    this.cnBox.classList.toggle('hidden', !v);
    if (!v) return;
    const head = h('div', 'section-head');
    head.append(h('div', 'label', 'Critical nets'), h('div', 'hint', 'tier 1 clocks · 2 links · 3 buses'));
    this.cnBox.append(head, h('div', 'hint', v.summary));
    const list = h('div', 'stall-list');
    for (const r of v.rows) {
      const row = h('div', `stall-row${r.name === selected ? ' on' : ''}`);
      const chip = h('span', 'chip-verdict', `T${r.tier}`);
      chip.style.background = r.warnings.length ? '#f2b84b' : '#5fd38d';
      chip.style.whiteSpace = 'nowrap';
      row.append(chip, h('span', 'stall-net', r.name), h('span', 'muted mono', r.unrouted ? `${r.unrouted} not routed` : `${r.vias} via${r.vias === 1 ? '' : 's'}${r.crossings ? ` · ${r.crossings} void${r.crossings === 1 ? '' : 's'}` : ''}`));
      row.title = [r.why, ...r.warnings].join('\n');
      row.onclick = () => this.onCriticalNet(r.name);
      list.appendChild(row);
      if (r.name === selected) queueMicrotask(() => row.scrollIntoView({ block: 'nearest' }));
    }
    this.cnBox.appendChild(list);
    const sel = v.rows.find((r) => r.name === selected);
    if (sel) {
      this.cnBox.appendChild(h('div', 'hint', sel.why));
      for (const w of sel.warnings) this.cnBox.appendChild(h('div', 'stall-label', w));
    }
  }

  /** The footprint-check list (issue #5): one row per footprint with something to say, errors first; a click selects it (and again deselects) and lists its issues. */
  setFootprintCheck(
    v: { summary: string; rows: { ref: string; libId: string; status: string; errors: number; warnings: number; datasheet: string; issues: { severity: string; text: string }[] }[] } | null,
    selected: string | null,
  ) {
    this.fcBox.textContent = '';
    this.fcBox.classList.toggle('hidden', !v);
    if (!v) return;
    const head = h('div', 'section-head');
    head.append(h('div', 'label', 'Footprint check'), h('div', 'hint', 'red differs from its datasheet · D toggles'));
    this.fcBox.append(head, h('div', 'hint', v.summary));
    const list = h('div', 'stall-list');
    for (const r of v.rows) {
      const row = h('div', `stall-row${r.ref === selected ? ' on' : ''}`);
      const chip = h('span', 'chip-verdict', r.errors ? `${r.errors} err` : r.status === 'untrusted' ? 'untrusted' : `${r.warnings} warn`);
      chip.style.background = r.errors ? '#ff5d6c' : '#f2b84b';
      chip.style.whiteSpace = 'nowrap';
      const lib = r.libId.split(':').pop() ?? r.libId;
      row.append(chip, h('span', 'stall-net', r.ref), h('span', 'muted mono', lib.length > 18 ? `${lib.slice(0, 17)}…` : lib));
      row.title = r.datasheet ? `${r.libId} vs ${r.datasheet}` : r.libId;
      row.onclick = () => this.onFootprintCheck(r.ref === selected ? null : r.ref);
      list.appendChild(row);
      if (r.ref === selected) queueMicrotask(() => row.scrollIntoView({ block: 'nearest' })); // a long list scrolls to the one that was picked
    }
    this.fcBox.appendChild(list);
    const sel = v.rows.find((r) => r.ref === selected);
    if (sel) {
      if (sel.datasheet) this.fcBox.appendChild(h('div', 'hint', `vs ${sel.datasheet}`));
      for (const i of sel.issues) this.fcBox.appendChild(h('div', i.severity === 'error' ? 'stall-label' : 'hint', i.text));
    }
  }

  /** The stall panel and banner (issue #8): null clears them. */
  setStall(v: StallView | null) {
    this.stallBox.textContent = '';
    this.banner.textContent = '';
    this.stallBox.classList.toggle('hidden', !v);
    this.banner.classList.toggle('hidden', !v);
    if (!v) return;
    const cur = v.rows[v.index];
    this.banner.append(
      h('span', 'stall-dot'),
      h('span', 'stall-title', `Stalled on ${cur?.net ?? '?'}`),
      h('span', 'muted', `${v.index + 1} of ${v.rows.length} · [ ] other connections · Space resumes`),
    );
    const head = h('div', 'section-head');
    const resume = h('button', 'btn', 'Resume');
    resume.title = 'Release the scene (Space)';
    resume.onclick = () => this.onStallDismiss();
    head.append(h('div', 'label', 'Stalled'), resume);
    this.stallBox.appendChild(head);
    const list = h('div', 'stall-list');
    v.rows.forEach((r, i) => {
      const row = h('div', `stall-row${i === v.index ? ' on' : ''}`);
      const chip = h('span', 'chip-verdict', r.verdict);
      chip.style.background = verdictColor(r.verdict);
      row.append(chip, h('span', 'stall-net', r.net), h('span', 'muted mono', `${r.reason || '–'} ×${r.failures}`));
      row.onclick = () => this.onStallPick(i);
      list.appendChild(row);
    });
    this.stallBox.append(list, h('div', 'stall-label', v.label), h('div', 'stall-verdict', v.verdict));
    if (v.rule) this.stallBox.appendChild(h('div', 'hint', v.rule));
  }

  setConnection(s: ConnState) {
    this.status.className = `status ${s}`;
    this.status.textContent = s === 'open' ? 'Live' : s === 'connecting' ? 'Connecting' : 'Offline';
  }

  private haveStats = false;

  setBoard(name: string, layers: string[], visible: boolean[], active: number) {
    this.boardName.textContent = name;
    if (!this.haveStats) {
      this.stageName.textContent = 'Board loaded';
      this.stageDetail.textContent = 'waiting for engine events';
    }
    document.title = `${name} · TraceMaker Live`;
    this.layerList.textContent = '';
    layers.forEach((ln, i) => {
      const row = h('div', 'layer-row');
      row.dataset.layer = String(i);
      const sw = h('span', 'swatch');
      sw.style.background = css(layerColor(i, layers.length));
      sw.style.boxShadow = `0 0 10px ${css(layerColor(i, layers.length))}55`;
      const name = h('span', 'layer-name', ln);
      const key = h('span', 'key', i < 9 ? String(i + 1) : '');
      const eye = h('button', 'eye');
      eye.title = 'Show / hide';
      eye.onclick = (e) => {
        e.stopPropagation();
        this.onLayerVisible(i, !row.classList.contains('visible'));
      };
      row.onclick = () => this.onActiveLayer(i);
      row.append(sw, name, key, eye);
      this.layerList.appendChild(row);
    });
    this.updateLayers(visible, active);
  }

  updateLayers(visible: boolean[], active: number) {
    for (const row of Array.from(this.layerList.children) as HTMLElement[]) {
      const i = Number(row.dataset.layer);
      row.classList.toggle('visible', !!visible[i]);
      row.classList.toggle('active', i === active);
    }
  }

  setToggles(t: Toggles) {
    const labels: [keyof Toggles, string, string][] = [
      ['activeOnTop', 'Active on top', 'H'], ['zones', 'Zones', 'Z'], ['ratsnest', 'Ratsnest', 'R'], ['escape', 'Escape plan', 'X'],
      ['footprints', 'Footprints', ''], ['effects', 'Effects', 'E'], ['follow', 'Follow activity', 'L'], ['returnPaths', 'Return paths', 'P'], ['footprintCheck', 'Footprint check', 'D'],
    ];
    this.chips.textContent = '';
    for (const [key, label, k] of labels) {
      const b = h('button', `chip${t[key] ? ' on' : ''}`, label);
      if (k) b.title = `Toggle (${k})`;
      b.onclick = () => this.onToggle(key, !t[key]);
      this.chips.appendChild(b);
    }
  }

  /** Overlay selector and legend: one chip per available heatmap (M cycles), a colour bar with the raw maximum. */
  setHeatmap(active: string, names: string[], max: number, scale: string) {
    this.heat.textContent = '';
    const row = h('div', 'heat-row');
    row.appendChild(h('span', 'label', 'Overlay'));
    for (const n of ['', ...names]) {
      const b = h('button', `chip${n === active ? ' on' : ''}`, n || 'Off');
      b.title = 'Heatmap overlay (M cycles)';
      b.onclick = () => this.onHeatmap(n);
      row.appendChild(b);
    }
    this.heat.appendChild(row);
    if (!names.length) {
      this.heat.appendChild(h('div', 'hint', 'no heatmaps yet (the router sends expansions and history)'));
      return;
    }
    if (!active) return;
    const bar = h('div', 'heat-bar');
    bar.style.background = infernoGradient();
    const ends = h('div', 'heat-ends');
    ends.append(h('span', 'mono', '0'), h('span', 'muted', scale === 'sqrt' ? 'square-root scale' : 'linear'), h('span', 'mono', num(max)));
    this.heat.append(bar, ends);
  }

  setStats(s: StatsMsg) {
    this.haveStats = true;
    this.stageName.textContent = s.stage || '—';
    this.iteration.textContent = num(s.iteration);
    this.routedText.textContent = `${num(s.routed)} / ${num(s.total)}`;
    const p = s.total > 0 ? (100 * s.routed) / s.total : 0;
    this.pct.textContent = s.total > 0 ? `${p.toFixed(1)}%` : '';
    this.bar.style.width = `${Math.min(100, p)}%`;
    this.bar.classList.toggle('done', s.total > 0 && s.routed >= s.total);
    this.cells.get('unrouted')!.textContent = num(s.unrouted);
    this.cells.get('rips')!.textContent = num(s.rips);
    this.cells.get('failures')!.textContent = num(s.failures);
    this.cells.get('elapsed')!.textContent = duration(s.elapsed_s);
    const ex = s.extra ? Object.entries(s.extra) : [];
    this.extra.textContent = '';
    for (const [k, v] of ex) {
      const row = h('div', 'kv');
      row.append(h('span', 'muted', k), h('span', 'mono', Number.isInteger(v) ? num(v) : v.toFixed(3)));
      this.extra.appendChild(row);
    }
  }

  setStage(name: string, state: string, detail: string) {
    if (state === 'begin') this.stageName.textContent = name;
    this.stageDetail.textContent = detail;
  }

  setRate(msgs: number, fps: number) {
    this.cells.get('rate')!.textContent = num(msgs);
    this.cells.get('fps')!.textContent = num(fps);
  }

  setCursor(x: number | null, y: number | null, zoomPxPerMm: number) {
    this.coords.textContent = x === null || y === null ? '' : `X ${x.toFixed(3)}  Y ${y.toFixed(3)} mm`;
    this.zoom.textContent = `${zoomPxPerMm >= 10 ? zoomPxPerMm.toFixed(0) : zoomPxPerMm.toFixed(2)} px/mm`;
  }

  addLog(level: string, text: string, kind: 'log' | 'failure' | 'stage' = 'log') {
    const atBottom = this.log.scrollHeight - this.log.scrollTop - this.log.clientHeight < 24;
    const row = h('div', `entry ${kind} lvl-${level}`);
    const d = new Date();
    const time = `${String(d.getHours()).padStart(2, '0')}:${String(d.getMinutes()).padStart(2, '0')}:${String(d.getSeconds()).padStart(2, '0')}`;
    row.append(h('span', 'time mono', time), h('span', 'badge', kind === 'log' ? level : kind), h('span', 'text', text));
    this.log.appendChild(row);
    if (++this.logCount > 400) {
      this.log.firstChild?.remove();
      this.logCount--;
    }
    if (atBottom) this.log.scrollTop = this.log.scrollHeight;
  }

  showTooltip(x: number, y: number, title: string, rows: [string, string][], color: string) {
    const t = this.tooltip;
    t.textContent = '';
    const head = h('div', 'tt-title');
    const dot = h('span', 'tt-dot');
    dot.style.background = color;
    head.append(dot, h('span', '', title));
    t.appendChild(head);
    for (const [k, v] of rows) {
      const r = h('div', 'kv');
      r.append(h('span', 'muted', k), h('span', 'mono', v));
      t.appendChild(r);
    }
    t.classList.add('show');
    const w = t.offsetWidth, hh = t.offsetHeight;
    const px = x + 16 + w > window.innerWidth - 340 ? x - w - 14 : x + 16;
    const py = Math.min(y + 18, window.innerHeight - hh - 8);
    t.style.transform = `translate(${Math.round(px)}px, ${Math.round(py)}px)`;
  }

  hideTooltip() { this.tooltip.classList.remove('show'); }

  fatal(msg: string) {
    if (this.fatalBox) return;
    this.fatalBox = h('div', 'panel fatal');
    this.fatalBox.append(h('h2', '', 'Cannot start the viewer'), h('p', '', msg));
    this.root.appendChild(this.fatalBox);
  }
}
