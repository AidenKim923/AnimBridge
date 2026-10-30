// Preview payloads are immutable after loading. Cards and the detail player share
// the same data, so expensive clip-wide bounds and parent lookup run only once.
const geometryCache = new WeakMap();

function geometryFor(data) {
  const cached = geometryCache.get(data);
  if (cached?.frames === data.frames && cached?.bones === data.bones) return cached;
  const lo = [Infinity, Infinity, Infinity],
    hi = [-Infinity, -Infinity, -Infinity];
  for (const frame of data.frames)
    for (const p of frame)
      for (let axis = 0; axis < 3; axis++) {
        lo[axis] = Math.min(lo[axis], p[axis]);
        hi[axis] = Math.max(hi[axis], p[axis]);
      }
  if (!Number.isFinite(lo[0])) {
    lo.fill(0);
    hi.fill(1);
  }
  const names = new Map();
  data.bones.forEach((bone, i) => {
    if (!names.has(bone.name)) names.set(bone.name, i);
  });
  const geometry = {
    frames: data.frames,
    bones: data.bones,
    center: lo.map((v, i) => (v + hi[i]) / 2),
    span: Math.max(...lo.map((v, i) => hi[i] - v), 0.001),
    base: lo[1],
    segments: data.bones
      .map((bone, i) => ({
        i,
        parent: typeof bone.parent === 'number' ? bone.parent : (names.get(bone.parent) ?? -1),
      }))
      .filter((bone) => bone.parent >= 0),
  };
  geometryCache.set(data, geometry);
  return geometry;
}

export class PoseViewer {
  constructor(canvas, data, options = {}) {
    this.canvas = canvas;
    this.ctx = canvas.getContext('2d');
    this.data = data;
    this.frame = 0;
    this.yaw = -0.45;
    this.pitch = 0.12;
    this.zoom = 1;
    this.floor = true;
    this.skeleton = true;
    this.drag = null;
    this.visible = options.visible !== false;
    this.lastDrawn = null;
    const geometry = geometryFor(data);
    this.center = geometry.center.slice();
    this.span = geometry.span;
    this.base = geometry.base;
    this.segments = geometry.segments;
    this.observer = new ResizeObserver(() => this.draw());
    this.observer.observe(canvas);
    // Chromium can discard a 2D canvas while the window is hidden; paint it again.
    canvas.addEventListener('contextrestored', (this.restored = () => this.repaint()));
    canvas.addEventListener(
      'pointerdown',
      (this.down = (e) => {
        this.drag = [e.clientX, e.clientY, this.yaw, this.pitch];
        canvas.setPointerCapture(e.pointerId);
      }),
    );
    canvas.addEventListener(
      'pointermove',
      (this.move = (e) => {
        if (this.drag) {
          this.yaw = this.drag[2] + (e.clientX - this.drag[0]) * 0.009;
          this.pitch = Math.max(-1, Math.min(1, this.drag[3] + (e.clientY - this.drag[1]) * 0.006));
          this.draw();
        }
      }),
    );
    canvas.addEventListener(
      'pointerup',
      (this.up = () => {
        this.drag = null;
      }),
    );
    canvas.addEventListener('pointercancel', this.up);
    canvas.addEventListener(
      'wheel',
      (this.wheel = (e) => {
        e.preventDefault();
        this.zoom = Math.max(0.35, Math.min(4, this.zoom * Math.exp(-e.deltaY * 0.001)));
        this.draw();
      }),
      { passive: false },
    );
    this.draw();
  }
  project(p, w, h) {
    let x = p[0] - this.center[0],
      y = p[1] - this.center[1],
      z = p[2] - this.center[2];
    const tx = x * Math.cos(this.yaw) + z * Math.sin(this.yaw),
      tz = -x * Math.sin(this.yaw) + z * Math.cos(this.yaw);
    let yy = y * Math.cos(this.pitch) - tz * Math.sin(this.pitch);
    let zz = y * Math.sin(this.pitch) + tz * Math.cos(this.pitch);
    const scale = (Math.min(w, h) * 0.68 * this.zoom) / this.span;
    return [w * 0.5 + tx * scale, h * 0.49 - yy * scale, zz, scale];
  }
  draw() {
    if (!this.visible) return;
    const { canvas: c, ctx: g } = this;
    const w = c.clientWidth,
      h = c.clientHeight;
    if (!w || !h) return;
    const dpr = Math.min(globalThis.devicePixelRatio || 1, 2),
      bitmapWidth = Math.round(w * dpr),
      bitmapHeight = Math.round(h * dpr);
    const signature = [this.frame, this.yaw, this.pitch, this.zoom, this.floor, this.skeleton,
      w, h, dpr, ...this.center, this.span, this.base];
    if (c.width === bitmapWidth && c.height === bitmapHeight &&
        this.lastDrawn?.every((value, i) => value === signature[i])) return;
    // Assigning canvas.width/height clears the bitmap and reallocates its backing
    // store. Animation frames only need a fresh transform and repaint.
    if (c.width !== bitmapWidth) c.width = bitmapWidth;
    if (c.height !== bitmapHeight) c.height = bitmapHeight;
    g.setTransform(dpr, 0, 0, dpr, 0, 0);
    g.fillStyle = '#141414';
    g.fillRect(0, 0, w, h);
    const glow = g.createRadialGradient(
      w * 0.5,
      h * 0.55,
      0,
      w * 0.5,
      h * 0.5,
      Math.max(w, h) * 0.7,
    );
    glow.addColorStop(0, '#232323');
    glow.addColorStop(1, '#0d0d0d');
    g.fillStyle = glow;
    g.fillRect(0, 0, w, h);
    const line = (a, b, color, width = 1) => {
      const aa = this.project(a, w, h),
        bb = this.project(b, w, h);
      g.strokeStyle = color;
      g.lineWidth = width;
      g.beginPath();
      g.moveTo(aa[0], aa[1]);
      g.lineTo(bb[0], bb[1]);
      g.stroke();
    };
    if (this.floor) {
      const n = 12,
        step = this.span / 5;
      for (let i = -n; i <= n; i++) {
        line(
          [this.center[0] + i * step, this.base, this.center[2] - n * step],
          [this.center[0] + i * step, this.base, this.center[2] + n * step],
          i === 0 ? '#484848' : '#2b2b2b',
        );
        line(
          [this.center[0] - n * step, this.base, this.center[2] + i * step],
          [this.center[0] + n * step, this.base, this.center[2] + i * step],
          i === 0 ? '#484848' : '#2b2b2b',
        );
      }
    }
    const mesh = this.data.mesh,
      vertices = mesh?.frames?.[Math.min(this.frame, mesh.frames.length - 1)];
    if (vertices) {
      const projected = vertices.map((p) => this.project(p, w, h));
      const triangles = mesh.triangles
        .map((t) => ({ t, z: t.reduce((sum, i) => sum + projected[i][2], 0) / 3 }))
        .sort((a, b) => b.z - a.z);
      for (const { t } of triangles) {
        const [a, b, c] = t.map((i) => vertices[i]),
          u = b.map((v, i) => v - a[i]),
          v = c.map((v, i) => v - a[i]),
          n = [u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]],
          len = Math.hypot(...n) || 1;
        const shade = Math.round(
          105 + 100 * Math.abs((n[0] * 0.25 + n[1] * 0.8 + n[2] * 0.54) / len),
        );
        g.fillStyle = `rgb(${shade},${shade},${shade})`;
        g.beginPath();
        t.forEach((i, j) => {
          const p = projected[i];
          j ? g.lineTo(p[0], p[1]) : g.moveTo(p[0], p[1]);
        });
        g.closePath();
        g.fill();
      }
    }
    const f = this.data.frames[Math.min(this.frame, this.data.frames.length - 1)];
    if (!f || !this.skeleton) {
      this.lastDrawn = signature;
      return;
    }
    g.lineCap = 'round';
    const projected = f.map((point) => this.project(point, w, h));
    const segments = this.segments.filter((bone) => f[bone.i] && f[bone.parent]);
    segments.sort((a, b) => projected[b.i][2] - projected[a.i][2]);
    for (const b of segments) {
      const a = projected[b.parent],
        v = projected[b.i];
      const width = Math.max(3, Math.min(13, this.span * 0.022 * a[3]));
      g.strokeStyle = '#555';
      g.lineWidth = width + 3;
      g.beginPath();
      g.moveTo(a[0], a[1]);
      g.lineTo(v[0], v[1]);
      g.stroke();
      const grad = g.createLinearGradient(a[0], a[1], v[0], v[1]);
      grad.addColorStop(0, '#d2d2d2');
      grad.addColorStop(1, '#878787');
      g.strokeStyle = grad;
      g.lineWidth = width;
      g.stroke();
    }
    projected.forEach((v, i) => {
      g.beginPath();
      g.arc(v[0], v[1], Math.max(2, Math.min(5, this.span * 0.009 * v[3])), 0, Math.PI * 2);
      g.fillStyle = i === 0 ? '#fff' : '#c8c8c8';
      g.fill();
    });
    this.lastDrawn = signature;
  }
  // Draw even if nothing changed (draw() skips identical frames).
  repaint() {
    this.lastDrawn = null;
    this.draw();
  }
  setFrame(frame) {
    this.frame = Math.max(0, Math.min(this.data.frames.length - 1,
      Number.isFinite(frame) ? Math.floor(frame) : 0));
    this.draw();
  }
  setVisible(visible) {
    if (this.visible === !!visible) return;
    this.visible = !!visible;
    if (this.visible) this.draw();
  }
  reset() {
    this.yaw = -0.45;
    this.pitch = 0.12;
    this.zoom = 1;
    this.draw();
  }
  destroy() {
    this.observer.disconnect();
    this.canvas.removeEventListener('pointerdown', this.down);
    this.canvas.removeEventListener('contextrestored', this.restored);
    this.canvas.removeEventListener('pointermove', this.move);
    this.canvas.removeEventListener('pointerup', this.up);
    this.canvas.removeEventListener('pointercancel', this.up);
    this.canvas.removeEventListener('wheel', this.wheel);
  }
}
