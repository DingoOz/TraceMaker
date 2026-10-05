// TraceMaker live viewer: connects to the engine's event stream and renders the board with WebGL2.
import './style.css';
import { Camera } from './camera';
import { css, layerColor, theme } from './colors';
import { Connection } from './connection';
import { Hud, num, type Toggles } from './hud';
import { Picker, type Hit } from './picking';
import { Renderer, type ViewOptions } from './renderer';
import { Scene } from './scene';

const PANEL_PX = 340; // side panel width incl. margin, kept clear when fitting the board

const canvas = document.getElementById('view') as HTMLCanvasElement;
const labels = document.createElement('canvas');
labels.id = 'labels';
document.body.appendChild(labels);
const hud = new Hud(document.getElementById('app')!);
const scene = new Scene();
const cam = new Camera();
const picker = new Picker();

let renderer: Renderer | null = null;
try {
  renderer = new Renderer(canvas);
} catch (e) {
  hud.fatal(`${(e as Error).message}. TraceMaker needs WebGL2 (Chrome, Edge, Firefox or Safari with hardware acceleration enabled).`);
}

// A lost GPU context (driver reset, too many tabs) is recovered by reloading: all state comes from the server.
canvas.addEventListener('webglcontextlost', (e) => e.preventDefault());
canvas.addEventListener('webglcontextrestored', () => location.reload());

const toggles: Toggles = { activeOnTop: true, zones: true, ratsnest: true, escape: true, footprints: true, effects: true, follow: false };
const view: ViewOptions = { visible: [], active: 0, activeOnTop: true, zones: true, ratsnest: true, escape: true, footprints: true, effects: true, hoverNet: 0, heatmap: '' };

const conn = new Connection(Connection.defaultUrl());
conn.onState = (s) => {
  hud.setConnection(s);
  if (s === 'closed') hud.addLog('warn', 'connection lost; reconnecting…');
};
hud.setConnection(conn.state);
hud.setToggles(toggles);

let dirty = true;
let labelsDirty = true;
let fitted = '';
let mouse: { x: number; y: number } | null = null;
let hoverMoved = false;

function applyToggles() {
  view.activeOnTop = toggles.activeOnTop;
  view.zones = toggles.zones;
  view.ratsnest = toggles.ratsnest;
  view.escape = toggles.escape;
  view.footprints = toggles.footprints;
  view.effects = toggles.effects;
  scene.glowEnabled = toggles.effects;
  hud.setToggles(toggles);
  dirty = labelsDirty = true;
}

hud.onToggle = (k, v) => { toggles[k] = v; applyToggles(); };
hud.onLayerVisible = (l, v) => { view.visible[l] = v; hud.updateLayers(view.visible, view.active); dirty = true; };
hud.onActiveLayer = (l) => {
  view.active = l;
  view.visible[l] = true;
  hud.updateLayers(view.visible, view.active);
  dirty = true;
};
hud.onFit = () => { cam.fit(scene.bbox, 0.06, PANEL_PX); dirty = labelsDirty = true; };

// Heatmap overlays (doc 09 §3): the router sends "expansions" (search effort) and "history" (PathFinder cost);
// any other name the engine sends is listed too. The choice survives a reconnect (kept by name).
let heatNames = '';
function updateHeatHud() {
  const names = [...scene.heatmaps.keys()].sort();
  heatNames = names.join('\n');
  const hm = scene.heatmaps.get(view.heatmap);
  hud.setHeatmap(view.heatmap, names, hm?.max ?? 0, hm?.scale ?? '');
}
hud.onHeatmap = (name) => { view.heatmap = name; updateHeatHud(); dirty = true; };
function cycleHeatmap() {
  const names = ['', ...[...scene.heatmaps.keys()].sort()];
  hud.onHeatmap(names[(names.indexOf(view.heatmap) + 1) % names.length]);
}

// ------------------------------------------------------------------------------------------------ input
let drag: { x: number; y: number; id: number } | null = null;
canvas.addEventListener('pointerdown', (e) => {
  drag = { x: e.clientX, y: e.clientY, id: e.pointerId };
  canvas.setPointerCapture(e.pointerId);
  canvas.classList.add('dragging');
});
canvas.addEventListener('pointermove', (e) => {
  mouse = { x: e.clientX, y: e.clientY };
  hoverMoved = true;
  if (drag && drag.id === e.pointerId) {
    cam.pan(e.clientX - drag.x, e.clientY - drag.y);
    drag.x = e.clientX;
    drag.y = e.clientY;
    dirty = labelsDirty = true;
  }
});
const endDrag = (e: PointerEvent) => {
  if (drag && drag.id === e.pointerId) drag = null;
  canvas.classList.remove('dragging');
};
canvas.addEventListener('pointerup', endDrag);
canvas.addEventListener('pointercancel', endDrag);
canvas.addEventListener('pointerleave', () => { mouse = null; hoverMoved = true; });
canvas.addEventListener('wheel', (e) => {
  e.preventDefault();
  const dy = e.deltaMode === 1 ? e.deltaY * 33 : e.deltaMode === 2 ? e.deltaY * 400 : e.deltaY;
  cam.zoomAt(e.clientX, e.clientY, Math.exp(-dy * 0.0016));
  dirty = labelsDirty = true;
}, { passive: false });
canvas.addEventListener('dblclick', () => hud.onFit());
window.addEventListener('keydown', (e) => {
  if (e.target instanceof HTMLInputElement || e.ctrlKey || e.metaKey || e.altKey) return;
  const k = e.key.toLowerCase();
  if (k === 'f') hud.onFit();
  else if (k === 'h') hud.onToggle('activeOnTop', !toggles.activeOnTop);
  else if (k === 'z') hud.onToggle('zones', !toggles.zones);
  else if (k === 'r') hud.onToggle('ratsnest', !toggles.ratsnest);
  else if (k === 'x') hud.onToggle('escape', !toggles.escape);
  else if (k === 'e') hud.onToggle('effects', !toggles.effects);
  else if (k === 'l') hud.onToggle('follow', !toggles.follow);
  else if (k === 'm') cycleHeatmap();
  else if (/^[1-9]$/.test(k) && Number(k) <= scene.layerNames.length) hud.onActiveLayer(Number(k) - 1);
});
const resize = () => {
  cam.width = window.innerWidth;
  cam.height = window.innerHeight;
  const dpr = window.devicePixelRatio || 1;
  labels.width = Math.round(cam.width * dpr);
  labels.height = Math.round(cam.height * dpr);
  dirty = labelsDirty = true;
};
window.addEventListener('resize', resize);
resize();

// A view can be bookmarked or shared as #view=<x mm>,<y mm>,<px per mm> (KiCad board coordinates);
// #layer=<n> selects the active layer, #hc=0 turns "active on top" off, #heat=<name> shows a heatmap overlay.
function applyHashView() {
  const p = new URLSearchParams(location.hash.slice(1));
  const v = p.get('view')?.split(',').map(Number);
  if (v && v.length === 3 && v.every(Number.isFinite)) {
    cam.cx = v[0] - scene.ox * 1e-6;
    cam.cy = v[1] - scene.oy * 1e-6;
    cam.zoom = v[2];
    cam.zoomAt(cam.width / 2, cam.height / 2, 1);
  }
  const l = Number(p.get('layer'));
  if (p.has('layer') && l >= 0 && l < scene.layerNames.length) hud.onActiveLayer(l);
  if (p.get('hc') === '0') hud.onToggle('activeOnTop', false);
  if (p.get('follow') === '1') hud.onToggle('follow', true);
  if (p.get('heat')) hud.onHeatmap(p.get('heat')!); // shown once the engine sends that overlay
}

// ----------------------------------------------------------------------------------------------- messages
let msgCount = 0;
function processMessages(now: number) {
  const msgs = conn.drain();
  if (!msgs.length) return;
  msgCount += msgs.length;
  // After a stall (background tab, slow GPU) a frame can carry seconds of transient messages; only the most
  // recent ones are still meaningful, so skip the rest instead of piling up stale effects.
  let transient = 0, state = 0;
  for (const m of msgs) {
    if (m.type === 'frontier' || m.type === 'path_try') transient++;
    else if (m.type === 'track_add' || m.type === 'failure' || m.type === 'board') state++;
  }
  // A burst of state (snapshot catch-up, or a stall) is applied without per-item effects.
  if (state > 40) scene.quietUntil = Math.max(scene.quietUntil, now + 0.05);
  let skip = Math.max(0, transient - 48);
  for (const m of msgs) {
    if (skip > 0 && (m.type === 'frontier' || m.type === 'path_try')) {
      skip--;
      continue;
    }
    scene.apply(m, now);
    switch (m.type) {
      case 'board': {
        scene.quietUntil = now + 0.75; // the server replays state after the snapshot
        view.visible = scene.layerNames.map(() => true);
        view.active = 0;
        hud.setBoard(scene.name, scene.layerNames, view.visible, view.active);
        updateHeatHud(); // overlays were cleared with the snapshot; the server resends them
        const key = `${scene.name}:${scene.bbox.join(',')}`;
        if (fitted !== key) {
          cam.fit(scene.bbox, 0.06, PANEL_PX);
          if (fitted === '') applyHashView();
          fitted = key;
        }
        hud.addLog('info', `board ${m.name}: ${num(m.pads.length)} pads, ${num(m.tracks.length)} tracks, ${num(m.vias.length)} vias, ${m.layers.length} layers`, 'stage');
        break;
      }
      case 'stats': hud.setStats(m); break;
      case 'stage':
        hud.setStage(m.name, m.state, m.detail ?? '');
        hud.addLog('info', `${m.name} ${m.state}${m.detail ? ` — ${m.detail}` : ''}`, 'stage');
        break;
      case 'log': hud.addLog(m.level, m.text); break;
      case 'failure':
        hud.addLog('error', `${scene.netName(m.net)}: ${m.cause}${m.rung ? ` (rung ${m.rung})` : ''}`, 'failure');
        break;
      case 'heatmap':
        // The legend changes only when the list of overlays or the shown one's maximum does.
        if (m.name === view.heatmap || [...scene.heatmaps.keys()].sort().join('\n') !== heatNames) updateHeatHud();
        break;
      case 'escape_plan':
        hud.addLog('info', `escape plan: ${m.corridors.length} pin corridors reserved`, 'stage');
        break;
      case 'escape_dead':
        hud.addLog('error', `${m.pad} (${scene.netName(m.net)}): cannot escape: ${m.why}`, 'failure');
        break;
      default: break;
    }
  }
  dirty = labelsDirty = true;
}

// --------------------------------------------------------------------------------------------------- hover
function describe(hit: Hit): { title: string; rows: [string, string][]; color: string } {
  const n = scene.layerNames.length;
  const mm = (v: number) => `${v.toFixed(3)} mm`;
  if (hit.kind === 'track') {
    const t = hit.item;
    const len = Math.hypot(t.bx - t.ax, t.by - t.ay);
    return { title: scene.netName(t.net), color: css(layerColor(t.layer, n)), rows: [['Track', `#${t.id}`], ['Layer', scene.layerNames[t.layer] ?? String(t.layer)], ['Width', mm(2 * t.r)], ['Length', mm(len)]] };
  }
  if (hit.kind === 'via') {
    const v = hit.item;
    return { title: scene.netName(v.net), color: css(theme.via), rows: [['Via', `#${v.id}`], ['Layers', `${scene.layerNames[v.top] ?? v.top} – ${scene.layerNames[v.bottom] ?? v.bottom}`], ['Size', `${mm(2 * v.r)} / ${mm(2 * v.rh)}`]] };
  }
  const p = hit.item;
  const fp = scene.footprints[p.fp];
  const layers = p.layers.length > 1 ? 'through-hole' : p.layers.length ? scene.layerNames[p.layers[0]] : 'no copper';
  const color = p.layers.length > 1 ? css(theme.padThrough) : css(layerColor(p.layers[0] ?? 0, n));
  const rows: [string, string][] = [['Pad', fp ? `${fp.ref}.${p.num}` : p.num || `#${p.id}`], ['Layers', layers]];
  if (fp?.value) rows.push(['Value', fp.value]);
  return { title: scene.netName(p.net), color, rows };
}

function updateHover() {
  if (!mouse || drag) {
    hud.hideTooltip();
    if (view.hoverNet !== 0) { view.hoverNet = 0; dirty = true; }
    hud.setCursor(null, null, cam.zoom);
    return;
  }
  const [wx, wy] = cam.toWorld(mouse.x, mouse.y);
  hud.setCursor(wx + scene.ox * 1e-6, wy + scene.oy * 1e-6, cam.zoom);
  let hit = picker.pick(scene, wx, wy, 3 / cam.zoom, view.visible, view.active);
  if (hit?.kind === 'track' && scene.tracks.get(hit.item.id) !== hit.item) hit = null;
  if (hit?.kind === 'via' && scene.vias.get(hit.item.id) !== hit.item) hit = null;
  const net = hit ? hit.item.net : 0;
  if (net !== view.hoverNet) { view.hoverNet = net; dirty = true; }
  if (hit) {
    const d = describe(hit);
    hud.showTooltip(mouse.x, mouse.y, d.title, d.rows, d.color);
  } else hud.hideTooltip();
}

// ---------------------------------------------------------------------------------------------- labels
function drawLabels() {
  const ctx = labels.getContext('2d');
  if (!ctx) return;
  const dpr = window.devicePixelRatio || 1;
  ctx.setTransform(1, 0, 0, 1, 0, 0);
  ctx.clearRect(0, 0, labels.width, labels.height);
  if (!scene.loaded || !toggles.footprints) return;
  ctx.setTransform(dpr, 0, 0, dpr, 0, 0);
  ctx.textAlign = 'center';
  ctx.textBaseline = 'middle';
  for (const f of scene.footprints) {
    const [x0, y0] = cam.toScreen(f.bbox[0], f.bbox[1]);
    const [x1, y1] = cam.toScreen(f.bbox[2], f.bbox[3]);
    const w = x1 - x0, hgt = y1 - y0;
    if (w < 26 || x1 < 0 || y1 < 0 || x0 > cam.width || y0 > cam.height) continue;
    const size = Math.min(15, Math.max(9, Math.min(w / Math.max(f.ref.length, 2) * 0.9, hgt * 0.45)));
    ctx.font = `600 ${size}px "Inter", system-ui, -apple-system, "Segoe UI", sans-serif`;
    ctx.fillStyle = f.back ? 'rgba(170,190,230,0.45)' : 'rgba(225,232,248,0.78)';
    ctx.shadowColor = 'rgba(0,0,0,0.85)';
    ctx.shadowBlur = 4;
    ctx.fillText(f.ref, (x0 + x1) / 2, (y0 + y1) / 2);
  }
}

// ----------------------------------------------------------------------------------------------- loop
let last = performance.now();
let frames = 0;
let rateT = last;
let animating = false;
let lastError = '';
function frame(tms: number) {
  requestAnimationFrame(frame);
  try {
    step(tms);
  } catch (e) {
    // Keep the loop alive; report each distinct error once.
    const msg = (e as Error)?.stack ?? String(e);
    if (msg !== lastError) {
      lastError = msg;
      console.error(e);
      hud.addLog('error', `viewer: ${(e as Error)?.message ?? e}`);
    }
  }
}

function step(tms: number) {
  const now = tms / 1000;
  const dt = Math.min(0.1, (tms - last) / 1000);
  last = tms;
  processMessages(now);
  if (toggles.follow && scene.activity && now - scene.activity.t < 1.5) {
    const [sx, sy] = cam.toScreen(scene.activity.x, scene.activity.y);
    const w = cam.width - PANEL_PX;
    if (sx < w * 0.2 || sx > w * 0.8 || sy < cam.height * 0.2 || sy > cam.height * 0.8) cam.followTo(scene.activity.x + PANEL_PX / 2 / cam.zoom, scene.activity.y);
  }
  if (cam.update(dt)) dirty = labelsDirty = true;
  if (hoverMoved || dirty) {
    hoverMoved = false;
    updateHover();
  }
  if (renderer && (dirty || animating)) {
    animating = renderer.render(scene, cam, view, now);
    dirty = false;
    frames++;
  }
  if (labelsDirty) {
    labelsDirty = false;
    drawLabels();
  }
  if (tms - rateT >= 1000) {
    const s = (tms - rateT) / 1000;
    hud.setRate(msgCount / s, frames / s);
    msgCount = 0;
    frames = 0;
    rateT = tms;
  }
}
requestAnimationFrame(frame);
