// HTML overlay: brand bar, progress, layer controls, log, status bar and hover tooltip.
import { css, layerColor } from './colors';
import type { ConnState } from './connection';
import type { StatsMsg } from './protocol';

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

export interface Toggles {
  activeOnTop: boolean;
  zones: boolean;
  ratsnest: boolean;
  footprints: boolean;
  effects: boolean;
  follow: boolean;
}

const LOGO = `<svg viewBox="0 0 32 32" width="22" height="22" aria-hidden="true"><rect width="32" height="32" rx="8" fill="#101a33"/>
<path d="M6 22h7.5l5-8H26" stroke="#e8545c" stroke-width="2.6" fill="none" stroke-linecap="round" stroke-linejoin="round"/>
<circle cx="26" cy="14" r="3.2" fill="#5b9cf5"/><circle cx="6" cy="22" r="2.2" fill="#d4ad3f"/></svg>`;

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
  private log = h('div', 'log');
  private logCount = 0;
  private coords = h('span', 'mono', '');
  private zoom = h('span', 'mono', '');
  readonly tooltip = h('div', 'tooltip');
  private fatalBox: HTMLElement | null = null;

  onLayerVisible: (layer: number, visible: boolean) => void = () => {};
  onActiveLayer: (layer: number) => void = () => {};
  onToggle: (key: keyof Toggles, value: boolean) => void = () => {};
  onFit: () => void = () => {};

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
    layers.append(lh, this.layerList, this.chips);

    const logSec = h('section', 'section grow');
    const logHead = h('div', 'section-head');
    const clear = h('button', 'link', 'Clear');
    clear.onclick = () => { this.log.textContent = ''; this.logCount = 0; };
    logHead.append(h('div', 'label', 'Events'), clear);
    logSec.append(logHead, this.log);

    side.append(prog, layers, logSec);
    this.root.appendChild(side);

    // Status bar.
    const sb = h('footer', 'panel statusbar');
    const fit = h('button', 'btn', 'Fit');
    fit.title = 'Fit board (F)';
    fit.onclick = () => this.onFit();
    sb.append(fit, this.coords, this.zoom, h('span', 'hint', 'drag to pan · wheel to zoom · F fit · H high contrast'));
    this.root.appendChild(sb);
    this.root.appendChild(this.tooltip);
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
      ['activeOnTop', 'Active on top', 'H'], ['zones', 'Zones', 'Z'], ['ratsnest', 'Ratsnest', 'R'],
      ['footprints', 'Footprints', ''], ['effects', 'Effects', 'E'], ['follow', 'Follow activity', 'L'],
    ];
    this.chips.textContent = '';
    for (const [key, label, k] of labels) {
      const b = h('button', `chip${t[key] ? ' on' : ''}`, label);
      if (k) b.title = `Toggle (${k})`;
      b.onclick = () => this.onToggle(key, !t[key]);
      this.chips.appendChild(b);
    }
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
