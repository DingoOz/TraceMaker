// 2-D camera in CSS pixels: pan by drag, smooth zoom about the cursor, fit to a box.
export class Camera {
  cx = 0; // world point at the viewport centre (mm)
  cy = 0;
  zoom = 4; // CSS px per mm
  width = 1; // viewport size in CSS px
  height = 1;
  private target = 4;
  private anchor: { sx: number; sy: number; wx: number; wy: number } | null = null;
  private follow: { x: number; y: number } | null = null;

  toScreen(x: number, y: number): [number, number] {
    return [(x - this.cx) * this.zoom + this.width / 2, (y - this.cy) * this.zoom + this.height / 2];
  }

  toWorld(sx: number, sy: number): [number, number] {
    return [(sx - this.width / 2) / this.zoom + this.cx, (sy - this.height / 2) / this.zoom + this.cy];
  }

  fit(b: [number, number, number, number], margin = 0.06, panelPx = 0) {
    const w = Math.max(b[2] - b[0], 1e-3), h = Math.max(b[3] - b[1], 1e-3);
    const avail = Math.max(this.width - panelPx, this.width * 0.4);
    this.zoom = this.target = Math.min(avail / (w * (1 + 2 * margin)), this.height / (h * (1 + 2 * margin)));
    // Centre the board in the area left of the side panel.
    this.cx = (b[0] + b[2]) / 2 + panelPx / 2 / this.zoom;
    this.cy = (b[1] + b[3]) / 2;
    this.anchor = null;
    this.follow = null;
  }

  pan(dx: number, dy: number) {
    this.cx -= dx / this.zoom;
    this.cy -= dy / this.zoom;
    this.anchor = null;
    this.follow = null;
  }

  zoomAt(sx: number, sy: number, factor: number) {
    const [wx, wy] = this.anchor && this.anchor.sx === sx && this.anchor.sy === sy ? [this.anchor.wx, this.anchor.wy] : this.toWorld(sx, sy);
    this.target = Math.min(Math.max(this.target * factor, 0.05), 20000);
    this.anchor = { sx, sy, wx, wy };
    this.follow = null;
  }

  /** Eases the centre towards a world point (follow-activity mode). */
  followTo(x: number, y: number) {
    if (this.anchor) return;
    this.follow = { x, y };
  }

  /** Advances animations by dt seconds; returns true if the view changed. */
  update(dt: number): boolean {
    let changed = false;
    if (this.anchor) {
      const k = 1 - Math.exp(-dt * 16);
      const lz = Math.log(this.zoom), lt = Math.log(this.target);
      if (Math.abs(lt - lz) < 1e-3) {
        this.zoom = this.target;
      } else {
        this.zoom = Math.exp(lz + (lt - lz) * k);
      }
      const a = this.anchor;
      this.cx = a.wx - (a.sx - this.width / 2) / this.zoom;
      this.cy = a.wy - (a.sy - this.height / 2) / this.zoom;
      if (this.zoom === this.target) this.anchor = null;
      changed = true;
    }
    if (this.follow) {
      const k = 1 - Math.exp(-dt * 2.5);
      const dx = this.follow.x - this.cx, dy = this.follow.y - this.cy;
      this.cx += dx * k;
      this.cy += dy * k;
      if (Math.hypot(dx, dy) * this.zoom < 0.5) this.follow = null;
      changed = true;
    }
    return changed;
  }
}
