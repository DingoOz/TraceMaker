// WebGL2 renderer: instanced SDF primitives, per-layer offscreen compositing, additive glow effects.
import type { Camera } from './camera';
import { layerColor, lighten, theme, type RGBA } from './colors';
import type { Pad, Scene } from './scene';
import * as S from './shaders';

type GL = WebGL2RenderingContext;

class Program {
  readonly prog: WebGLProgram;
  private readonly loc = new Map<string, WebGLUniformLocation | null>();
  constructor(private readonly gl: GL, vs: string, fs: string) {
    const compile = (type: number, src: string) => {
      const s = gl.createShader(type)!;
      gl.shaderSource(s, src);
      gl.compileShader(s);
      if (!gl.getShaderParameter(s, gl.COMPILE_STATUS)) throw new Error(`shader: ${gl.getShaderInfoLog(s)}`);
      return s;
    };
    const p = gl.createProgram()!;
    gl.attachShader(p, compile(gl.VERTEX_SHADER, vs));
    gl.attachShader(p, compile(gl.FRAGMENT_SHADER, fs));
    gl.linkProgram(p);
    if (!gl.getProgramParameter(p, gl.LINK_STATUS)) throw new Error(`link: ${gl.getProgramInfoLog(p)}`);
    this.prog = p;
  }
  u(name: string): WebGLUniformLocation | null {
    if (!this.loc.has(name)) this.loc.set(name, this.gl.getUniformLocation(this.prog, name));
    return this.loc.get(name)!;
  }
}

/** A growable vertex buffer with a VAO; `layout` lists attribute sizes (floats), all instanced or none. */
class Batch {
  readonly vao: WebGLVertexArrayObject;
  private readonly vbo: WebGLBuffer;
  private capacity = 0;
  count = 0;
  private readonly stride: number;
  constructor(private readonly gl: GL, quad: WebGLBuffer | null, layout: number[], readonly instanced: boolean) {
    this.stride = layout.reduce((a, b) => a + b, 0);
    this.vao = gl.createVertexArray()!;
    this.vbo = gl.createBuffer()!;
    gl.bindVertexArray(this.vao);
    let first = 0;
    if (quad) {
      gl.bindBuffer(gl.ARRAY_BUFFER, quad);
      gl.enableVertexAttribArray(0);
      gl.vertexAttribPointer(0, 2, gl.FLOAT, false, 0, 0);
      first = 1;
    }
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    let off = 0;
    layout.forEach((size, i) => {
      const loc = first + i;
      gl.enableVertexAttribArray(loc);
      gl.vertexAttribPointer(loc, size, gl.FLOAT, false, this.stride * 4, off * 4);
      if (instanced) gl.vertexAttribDivisor(loc, 1);
      off += size;
    });
    gl.bindVertexArray(null);
  }
  set(data: Float32Array | number[]) {
    const gl = this.gl;
    const arr = data instanceof Float32Array ? data : new Float32Array(data);
    gl.bindBuffer(gl.ARRAY_BUFFER, this.vbo);
    if (arr.length > this.capacity) {
      this.capacity = Math.max(arr.length, Math.ceil(this.capacity * 1.5));
      gl.bufferData(gl.ARRAY_BUFFER, this.capacity * 4, gl.DYNAMIC_DRAW);
    }
    gl.bufferSubData(gl.ARRAY_BUFFER, 0, arr);
    this.count = arr.length / this.stride;
  }
  draw(mode?: number) {
    if (this.count === 0) return;
    const gl = this.gl;
    gl.bindVertexArray(this.vao);
    if (this.instanced) gl.drawArraysInstanced(gl.TRIANGLE_STRIP, 0, 4, this.count);
    else gl.drawArrays(mode ?? gl.TRIANGLES, 0, this.count);
  }
}

interface LayerBuffers {
  tracks: Batch;
  padCaps: Batch;
  padTris: Batch;
  zoneFan: Batch;
  zoneCover: Batch;
  hasZones: boolean;
}

export interface ViewOptions {
  visible: boolean[];
  active: number;
  activeOnTop: boolean;
  zones: boolean;
  ratsnest: boolean;
  escape: boolean;
  footprints: boolean;
  effects: boolean;
  hoverNet: number;
  heatmap: string; // overlay name ('' = none)
}

export class Renderer {
  readonly gl: GL;
  private capsule: Program;
  private ring: Program;
  private tri: Program;
  private comp: Program;
  private fx: Program;
  private heat: Program;
  private heatTex: WebGLTexture | null = null;
  private heatShown = ''; // name@version of the grid in heatTex
  private quad: WebGLBuffer;
  private layers: LayerBuffers[] = [];
  private thCaps: Batch;
  private thTris: Batch;
  private holes: Batch;
  private vias: Batch;
  private outlineFan: Batch;
  private outlineCover: Batch;
  private outlineLines: Batch;
  private fpLines: Batch;
  private rats: Batch;
  private esc: Batch;
  private dead: Batch;
  private fxBatch: Batch;
  private fullscreen: Batch;
  private boardVersion = -1;
  private msaaFbo: WebGLFramebuffer | null = null;
  private resolveFbo: WebGLFramebuffer | null = null;
  private colorRb: WebGLRenderbuffer | null = null;
  private dsRb: WebGLRenderbuffer | null = null;
  private tex: WebGLTexture | null = null;
  private fbW = 0;
  private fbH = 0;
  private samples = 4;
  fxCount = 0;

  constructor(private readonly canvas: HTMLCanvasElement) {
    const gl = canvas.getContext('webgl2', { antialias: false, stencil: true, alpha: false, premultipliedAlpha: true, powerPreference: 'high-performance' });
    if (!gl) throw new Error('WebGL2 is not available in this browser');
    this.gl = gl;
    this.capsule = new Program(gl, S.capsuleVS, S.capsuleFS);
    this.ring = new Program(gl, S.ringVS, S.ringFS);
    this.tri = new Program(gl, S.triVS, S.triFS);
    this.comp = new Program(gl, S.compositeVS, S.compositeFS);
    this.fx = new Program(gl, S.fxVS, S.fxFS);
    this.heat = new Program(gl, S.heatVS, S.heatFS);
    this.quad = gl.createBuffer()!;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.quad);
    gl.bufferData(gl.ARRAY_BUFFER, new Float32Array([-1, -1, 1, -1, -1, 1, 1, 1]), gl.STATIC_DRAW);
    this.thCaps = this.capsBatch();
    this.thTris = this.triBatch();
    this.holes = this.capsBatch();
    this.vias = new Batch(gl, this.quad, [4, 1], true);
    this.outlineFan = this.triBatch();
    this.outlineCover = this.triBatch();
    this.outlineLines = this.capsBatch();
    this.fpLines = this.capsBatch();
    this.rats = this.capsBatch();
    this.esc = this.capsBatch();
    this.dead = this.capsBatch();
    this.fxBatch = new Batch(gl, this.quad, [4, 4, 4, 4], true);
    this.fullscreen = new Batch(gl, this.quad, [], false);
    this.fullscreen.count = 4;
    this.samples = Math.min(4, gl.getParameter(gl.MAX_SAMPLES) as number);
  }

  private capsBatch() { return new Batch(this.gl, this.quad, [4, 2], true); }
  private triBatch() { return new Batch(this.gl, null, [2, 1], false); }

  // ------------------------------------------------------------------------------------------------ buffers
  private ensureLayers(n: number) {
    while (this.layers.length < n)
      this.layers.push({ tracks: this.capsBatch(), padCaps: this.capsBatch(), padTris: this.triBatch(), zoneFan: this.triBatch(), zoneCover: this.triBatch(), hasZones: false });
  }

  private static padGeometry(p: Pad, caps: number[], tris: number[]) {
    const pts = p.pts;
    if (p.shape === 'circle') caps.push(pts[0], pts[1], pts[0], pts[1], p.r, p.net);
    else if (p.shape === 'segment') caps.push(pts[0], pts[1], pts[2], pts[3], p.r, p.net);
    else {
      for (const i of p.tris ?? []) tris.push(pts[2 * i], pts[2 * i + 1], p.net);
      if (p.r > 0) {
        const n = pts.length / 2;
        for (let i = 0; i < n; i++) {
          const j = (i + 1) % n;
          caps.push(pts[2 * i], pts[2 * i + 1], pts[2 * j], pts[2 * j + 1], p.r, p.net);
        }
      }
    }
  }

  private static fan(polys: number[][], out: number[]) {
    for (const poly of polys) {
      const n = poly.length / 2;
      for (let i = 1; i + 1 < n; i++) out.push(poly[0], poly[1], 0, poly[2 * i], poly[2 * i + 1], 0, poly[2 * i + 2], poly[2 * i + 3], 0);
    }
  }

  private static cover(polys: number[][], out: number[]) {
    let x0 = Infinity, y0 = Infinity, x1 = -Infinity, y1 = -Infinity;
    for (const poly of polys)
      for (let i = 0; i < poly.length; i += 2) {
        x0 = Math.min(x0, poly[i]); x1 = Math.max(x1, poly[i]);
        y0 = Math.min(y0, poly[i + 1]); y1 = Math.max(y1, poly[i + 1]);
      }
    if (x0 > x1) return;
    out.push(x0, y0, 0, x1, y0, 0, x0, y1, 0, x1, y0, 0, x1, y1, 0, x0, y1, 0);
  }

  private sync(scene: Scene) {
    const n = scene.layerNames.length;
    this.ensureLayers(n);
    if (this.boardVersion !== scene.boardVersion) {
      this.boardVersion = scene.boardVersion;
      for (let l = 0; l < n; l++) {
        const polys = scene.zones.filter((z) => z.layer === l).flatMap((z) => z.polys);
        const fan: number[] = [], cover: number[] = [];
        Renderer.fan(polys, fan);
        Renderer.cover(polys, cover);
        this.layers[l].zoneFan.set(fan);
        this.layers[l].zoneCover.set(cover);
        this.layers[l].hasZones = polys.length > 0;
      }
      const fan: number[] = [], cover: number[] = [], lines: number[] = [];
      Renderer.fan(scene.outline, fan);
      Renderer.cover(scene.outline, cover);
      for (const loop of scene.outline)
        for (let i = 0; i + 3 < loop.length; i += 2) lines.push(loop[i], loop[i + 1], loop[i + 2], loop[i + 3], 0, 0);
      this.outlineFan.set(fan);
      this.outlineCover.set(cover);
      this.outlineLines.set(lines);
    }
    if (scene.dirtyTrackLayers.size) {
      const per: number[][] = Array.from({ length: n }, () => []);
      const dirty = scene.dirtyTrackLayers;
      for (const t of scene.tracks.values()) if (dirty.has(t.layer) && t.layer < n) per[t.layer].push(t.ax, t.ay, t.bx, t.by, t.r, t.net);
      for (const l of dirty) if (l < n) this.layers[l].tracks.set(per[l]);
      dirty.clear();
    }
    if (scene.padsDirty) {
      scene.padsDirty = false;
      const caps: number[][] = Array.from({ length: n }, () => []), tris: number[][] = Array.from({ length: n }, () => []);
      const thc: number[] = [], tht: number[] = [], holes: number[] = [];
      for (const p of scene.pads) {
        if (p.layers.length === 1 && p.layers[0] < n) Renderer.padGeometry(p, caps[p.layers[0]], tris[p.layers[0]]);
        else if (p.layers.length > 1) Renderer.padGeometry(p, thc, tht);
        if (p.hole) {
          const h = p.hole.pts;
          holes.push(h[0], h[1], h[2] ?? h[0], h[3] ?? h[1], p.hole.r, 0);
        }
      }
      for (let l = 0; l < n; l++) {
        this.layers[l].padCaps.set(caps[l]);
        this.layers[l].padTris.set(tris[l]);
      }
      this.thCaps.set(thc);
      this.thTris.set(tht);
      this.holes.set(holes);
    }
    if (scene.viasDirty) {
      scene.viasDirty = false;
      const v: number[] = [];
      for (const via of scene.vias.values()) v.push(via.x, via.y, via.r, via.rh, via.net);
      this.vias.set(v);
    }
    if (scene.footprintsDirty) {
      scene.footprintsDirty = false;
      const f: number[] = [];
      for (const fp of scene.footprints) {
        const [x0, y0, x1, y1] = fp.bbox;
        const m = 0.25; // courtyard-like margin around the pads (mm)
        const a = x0 - m, b = y0 - m, c = x1 + m, d = y1 + m;
        f.push(a, b, c, b, 0, 0, c, b, c, d, 0, 0, c, d, a, d, 0, 0, a, d, a, b, 0, 0);
      }
      this.fpLines.set(f);
    }
    if (scene.ratsDirty) {
      scene.ratsDirty = false;
      const r: number[] = [];
      for (let i = 0; i < scene.ratsnest.length; i += 4)
        r.push(scene.ratsnest[i], scene.ratsnest[i + 1], scene.ratsnest[i + 2], scene.ratsnest[i + 3], 0, scene.ratsNets[i / 4]);
      this.rats.set(r);
    }
    if (scene.escDirty) {
      scene.escDirty = false;
      const e: number[] = [];
      for (const c of scene.escapes.values())
        for (let i = 0; i + 3 < c.pts.length; i += 2) e.push(c.pts[i], c.pts[i + 1], c.pts[i + 2], c.pts[i + 3], 0.03, c.net);
      this.esc.set(e);
      const d: number[] = [];
      const a = 0.35;  // marker arm (mm)
      for (const p of scene.deadPins) d.push(p.x - a, p.y - a, p.x + a, p.y + a, 0.07, p.net, p.x - a, p.y + a, p.x + a, p.y - a, 0.07, p.net);
      this.dead.set(d);
    }
  }

  // Effects buffer, rebuilt every frame while anything is alive. Returns true if effects remain.
  private syncFx(scene: Scene, now: number, enabled: boolean): boolean {
    const fx = scene.fx;
    const LIFE_TRACK = 1.4, LIFE_DOT = 0.6, LIFE_PATH = 0.9, LIFE_FAIL = 4.5;
    fx.newTracks = fx.newTracks.filter((e) => now - e.t0 < LIFE_TRACK && scene.tracks.get(e.t.id) === e.t);
    fx.frontier = fx.frontier.filter((e) => now - e.t0 < LIFE_DOT);
    if (fx.frontier.length > 30000) fx.frontier = fx.frontier.slice(-30000);
    fx.paths = fx.paths.filter((e) => now - e.t0 < LIFE_PATH);
    fx.failures = fx.failures.filter((e) => now - e.t0 < LIFE_FAIL);
    const alive = fx.newTracks.length + fx.frontier.length + fx.paths.length + fx.failures.length;
    if (!enabled || alive === 0) {
      this.fxBatch.count = 0;
      this.fxCount = 0;
      return false;
    }
    const n = scene.layerNames.length;
    const d: number[] = [];
    const push = (ax: number, ay: number, bx: number, by: number, r: number, minPx: number, sigma: number, t0: number, life: number, kind: number, dash: number, c: RGBA, intensity: number) =>
      d.push(ax, ay, bx, by, r, minPx, sigma, t0, life, kind, dash, 0, c[0], c[1], c[2], intensity);
    for (const e of fx.newTracks) {
      const c = lighten(layerColor(e.t.layer, n), 0.35);
      push(e.t.ax, e.t.ay, e.t.bx, e.t.by, e.t.r, 1, 7, e.t0, LIFE_TRACK, 0, 0, c, 0.55);
      push(e.t.ax, e.t.ay, e.t.bx, e.t.by, e.t.r, 1, 22, e.t0, LIFE_TRACK, 0, 0, c, 0.16);
    }
    for (const p of fx.paths)
      for (let i = 0; i + 3 < p.pts.length; i += 2) push(p.pts[i], p.pts[i + 1], p.pts[i + 2], p.pts[i + 3], 0, 0.7, 2.6, p.t0, LIFE_PATH, 0, 0, theme.pathTry, 0.5);
    for (const f of fx.frontier) push(f.x, f.y, f.x, f.y, 0, 1.1, 2.6, f.t0, LIFE_DOT, 0, 0, theme.frontier, 0.42);
    for (const f of fx.failures) {
      push(f.ax, f.ay, f.bx, f.by, 0, 0.8, 1.8, f.t0, LIFE_FAIL, 2, 9, theme.failure, 1.0);
      push(f.ax, f.ay, f.ax, f.ay, 0.6, 9, 1.8, f.t0, LIFE_FAIL, 1, 0, theme.failure, 1.0);
      push(f.bx, f.by, f.bx, f.by, 0.6, 9, 1.8, f.t0, LIFE_FAIL, 1, 0, theme.failure, 1.0);
    }
    this.fxBatch.set(d);
    this.fxCount = this.fxBatch.count;
    return true;
  }

  // ------------------------------------------------------------------------------------------- framebuffers
  private ensureTargets(w: number, h: number) {
    if (w === this.fbW && h === this.fbH && this.msaaFbo) return;
    const gl = this.gl;
    this.fbW = w;
    this.fbH = h;
    if (this.msaaFbo) gl.deleteFramebuffer(this.msaaFbo);
    if (this.resolveFbo) gl.deleteFramebuffer(this.resolveFbo);
    if (this.colorRb) gl.deleteRenderbuffer(this.colorRb);
    if (this.dsRb) gl.deleteRenderbuffer(this.dsRb);
    if (this.tex) gl.deleteTexture(this.tex);
    this.colorRb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, this.colorRb);
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, this.samples, gl.RGBA8, w, h);
    this.dsRb = gl.createRenderbuffer();
    gl.bindRenderbuffer(gl.RENDERBUFFER, this.dsRb);
    gl.renderbufferStorageMultisample(gl.RENDERBUFFER, this.samples, gl.DEPTH24_STENCIL8, w, h);
    this.msaaFbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.msaaFbo);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.RENDERBUFFER, this.colorRb);
    gl.framebufferRenderbuffer(gl.FRAMEBUFFER, gl.DEPTH_STENCIL_ATTACHMENT, gl.RENDERBUFFER, this.dsRb);
    this.tex = gl.createTexture();
    gl.bindTexture(gl.TEXTURE_2D, this.tex);
    gl.texImage2D(gl.TEXTURE_2D, 0, gl.RGBA8, w, h, 0, gl.RGBA, gl.UNSIGNED_BYTE, null);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.NEAREST);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
    gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    this.resolveFbo = gl.createFramebuffer();
    gl.bindFramebuffer(gl.FRAMEBUFFER, this.resolveFbo);
    gl.framebufferTexture2D(gl.FRAMEBUFFER, gl.COLOR_ATTACHMENT0, gl.TEXTURE_2D, this.tex, 0);
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
  }

  // ------------------------------------------------------------------------------------------------- frame
  private setView(p: Program, cam: Camera, dpr: number) {
    const gl = this.gl;
    gl.useProgram(p.prog);
    const zoom = cam.zoom * dpr;
    gl.uniform4f(p.u('u_view'), cam.cx, cam.cy, (zoom * 2) / this.canvas.width, (zoom * 2) / this.canvas.height);
    gl.uniform1f(p.u('u_px'), 1 / zoom);
  }

  private colors(p: Program, c: RGBA, hl: number, hlColor?: RGBA) {
    const gl = this.gl;
    gl.uniform4fv(p.u('u_color'), c);
    gl.uniform4fv(p.u('u_hlColor'), hlColor ?? lighten(c, 0.6));
    gl.uniform1f(p.u('u_hl'), hl);
  }

  /** Even-odd fill of polygons with the stencil buffer: fans invert the stencil, the cover draws where set. */
  private stencilFill(fan: Batch, cover: Batch, color: RGBA, cam: Camera, dpr: number) {
    const gl = this.gl;
    this.setView(this.tri, cam, dpr);
    this.colors(this.tri, color, 0);
    gl.enable(gl.STENCIL_TEST);
    gl.colorMask(false, false, false, false);
    gl.stencilFunc(gl.ALWAYS, 0, 0xff);
    gl.stencilOp(gl.KEEP, gl.KEEP, gl.INVERT);
    gl.stencilMask(0x01);
    fan.draw();
    gl.colorMask(true, true, true, true);
    gl.stencilFunc(gl.EQUAL, 1, 0x01);
    gl.stencilOp(gl.ZERO, gl.ZERO, gl.ZERO);
    cover.draw();
    gl.disable(gl.STENCIL_TEST);
  }

  private drawCaps(b: Batch, cam: Camera, dpr: number, color: RGBA, hl: number, minPx: number, hlColor?: RGBA) {
    if (!b.count) return;
    this.setView(this.capsule, cam, dpr);
    this.colors(this.capsule, color, hl, hlColor);
    this.gl.uniform1f(this.capsule.u('u_minPx'), minPx);
    b.draw();
  }

  private drawTris(b: Batch, cam: Camera, dpr: number, color: RGBA, hl: number) {
    if (!b.count) return;
    this.setView(this.tri, cam, dpr);
    this.colors(this.tri, color, hl);
    b.draw();
  }

  /** Heatmap overlay over the copper: the grid is uploaded as an R8 texture only when a new one arrives. */
  private drawHeatmap(scene: Scene, name: string, cam: Camera, dpr: number) {
    const hm = scene.heatmaps.get(name);
    if (!hm) return;
    const gl = this.gl;
    if (!this.heatTex) {
      this.heatTex = gl.createTexture();
      gl.bindTexture(gl.TEXTURE_2D, this.heatTex);
      // Linear filtering blends neighbouring cells: a coarse grid reads as a smooth field, not as blocks.
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MIN_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_MAG_FILTER, gl.LINEAR);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_S, gl.CLAMP_TO_EDGE);
      gl.texParameteri(gl.TEXTURE_2D, gl.TEXTURE_WRAP_T, gl.CLAMP_TO_EDGE);
    }
    gl.activeTexture(gl.TEXTURE0);
    gl.bindTexture(gl.TEXTURE_2D, this.heatTex);
    const key = `${hm.name}@${hm.version}`;
    if (key !== this.heatShown) {
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 1);
      gl.texImage2D(gl.TEXTURE_2D, 0, gl.R8, hm.w, hm.h, 0, gl.RED, gl.UNSIGNED_BYTE, hm.data);
      gl.pixelStorei(gl.UNPACK_ALIGNMENT, 4);
      this.heatShown = key;
    }
    this.setView(this.heat, cam, dpr);
    gl.uniform4f(this.heat.u('u_rect'), hm.x0, hm.y0, hm.w * hm.cell, hm.h * hm.cell);
    gl.uniform1i(this.heat.u('u_tex'), 0);
    gl.uniform1f(this.heat.u('u_opacity'), 0.72);
    this.fullscreen.draw(gl.TRIANGLE_STRIP);
  }

  /** Renders one frame. Returns true while animations are running (effects). */
  render(scene: Scene, cam: Camera, opt: ViewOptions, now: number): boolean {
    const gl = this.gl;
    const dpr = window.devicePixelRatio || 1;
    const w = Math.max(1, Math.round(cam.width * dpr)), h = Math.max(1, Math.round(cam.height * dpr));
    if (this.canvas.width !== w || this.canvas.height !== h) {
      this.canvas.width = w;
      this.canvas.height = h;
    }
    this.sync(scene);
    const animating = this.syncFx(scene, now, opt.effects);
    this.ensureTargets(w, h);

    const bg = theme.background;
    gl.bindFramebuffer(gl.FRAMEBUFFER, null);
    gl.viewport(0, 0, w, h);
    gl.clearColor(bg[0], bg[1], bg[2], 1);
    gl.clearStencil(0);
    gl.stencilMask(0xff);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    if (!scene.loaded) return false;

    // Board substrate.
    this.stencilFill(this.outlineFan, this.outlineCover, theme.substrate, cam, dpr);

    // Copper layers, bottom first; in "active on top" mode the active layer comes last and others are dimmed.
    const n = scene.layerNames.length;
    let order = Array.from({ length: n }, (_, i) => n - 1 - i);
    if (opt.activeOnTop) order = order.filter((l) => l !== opt.active).concat(opt.active < n ? [opt.active] : []);
    for (const l of order) {
      if (!opt.visible[l]) continue;
      const L = this.layers[l];
      const c = layerColor(l, n);
      gl.bindFramebuffer(gl.FRAMEBUFFER, this.msaaFbo);
      gl.viewport(0, 0, w, h);
      gl.clearColor(0, 0, 0, 0);
      gl.stencilMask(0xff);
      gl.clear(gl.COLOR_BUFFER_BIT | gl.STENCIL_BUFFER_BIT);
      if (opt.zones && L.hasZones) this.stencilFill(L.zoneFan, L.zoneCover, [c[0] * 0.42, c[1] * 0.42, c[2] * 0.42, 0.42], cam, dpr);
      this.drawCaps(L.tracks, cam, dpr, c, opt.hoverNet, 0.5);
      this.drawCaps(L.padCaps, cam, dpr, c, opt.hoverNet, 0.5);
      this.drawTris(L.padTris, cam, dpr, c, opt.hoverNet);
      gl.bindFramebuffer(gl.READ_FRAMEBUFFER, this.msaaFbo);
      gl.bindFramebuffer(gl.DRAW_FRAMEBUFFER, this.resolveFbo);
      gl.blitFramebuffer(0, 0, w, h, 0, 0, w, h, gl.COLOR_BUFFER_BIT, gl.NEAREST);
      gl.bindFramebuffer(gl.FRAMEBUFFER, null);
      gl.viewport(0, 0, w, h);
      gl.useProgram(this.comp.prog);
      gl.activeTexture(gl.TEXTURE0);
      gl.bindTexture(gl.TEXTURE_2D, this.tex);
      gl.uniform1i(this.comp.u('u_tex'), 0);
      const alpha = opt.activeOnTop ? (l === opt.active ? 1 : 0.5) : 0.86;
      gl.uniform1f(this.comp.u('u_alpha'), alpha);
      this.fullscreen.draw(gl.TRIANGLE_STRIP);
    }

    const anyVisible = opt.visible.some((v, i) => v && i < n);
    if (anyVisible) {
      this.drawCaps(this.thCaps, cam, dpr, theme.padThrough, opt.hoverNet, 0.5);
      this.drawTris(this.thTris, cam, dpr, theme.padThrough, opt.hoverNet);
    }
    this.drawCaps(this.holes, cam, dpr, theme.hole, 0, 0);
    if (anyVisible && this.vias.count) {
      this.setView(this.ring, cam, dpr);
      this.colors(this.ring, theme.via, opt.hoverNet, lighten(theme.via, 0.7));
      gl.uniform4fv(this.ring.u('u_holeColor'), theme.hole);
      this.vias.draw();
    }
    if (opt.heatmap) this.drawHeatmap(scene, opt.heatmap, cam, dpr);
    if (opt.footprints) this.drawCaps(this.fpLines, cam, dpr, theme.footprint, 0, 0.5);
    this.drawCaps(this.outlineLines, cam, dpr, theme.outline, 0, 0.7);
    if (opt.ratsnest) this.drawCaps(this.rats, cam, dpr, theme.ratsnest, opt.hoverNet, 0.5, [1, 1, 1, 0.95]);
    if (opt.escape) {
      this.drawCaps(this.esc, cam, dpr, theme.escape, opt.hoverNet, 0.5, [1, 1, 1, 0.95]);
      this.drawCaps(this.dead, cam, dpr, theme.deadPin, 0, 0.5);
    }

    if (animating) {
      gl.blendFunc(gl.ONE, gl.ONE);
      this.setView(this.fx, cam, dpr);
      gl.uniform1f(this.fx.u('u_time'), now);
      this.fxBatch.draw();
      gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA);
    }
    return animating;
  }
}
