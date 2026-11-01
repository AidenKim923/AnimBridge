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

// Mesh triangles as one flat list of vertex indices [a, b, c, a, b, c, ...], built once per mesh.
const cornerCache = new WeakMap();

function meshCorners(triangles) {
  let corners = cornerCache.get(triangles);
  if (!corners) {
    corners = Uint32Array.from(triangles.flat());
    cornerCache.set(triangles, corners);
  }
  return corners;
}

// ---------------------------------------------------------------------------
// GPU drawing
//
// Chromium only keeps about 16 live WebGL contexts, but the library grid can show dozens
// of viewers. So all viewers share ONE hidden WebGL canvas: each viewer renders its frame
// there, then copies the pixels onto its own visible canvas with a 2D drawImage.
// Any failure (no WebGL, lost context, GL error) makes viewers use the Canvas 2D path.
// ---------------------------------------------------------------------------

const FLOOR_HALF_LINES = 12; // grid lines on each side of the center line
const MAX_QUADS = 16383; // quads use 16-bit indices: 4 vertices per quad

// Color values used by both the Canvas 2D path and the shaders.
const COLORS = {
  gridLine: '#2b2b2b',
  gridCenter: '#484848',
  boneOutline: '#555555',
  boneStart: '#d2d2d2',
  boneEnd: '#878787',
  rootJoint: '#ffffff',
  joint: '#c8c8c8',
};

function rgb(hex) {
  const n = parseInt(hex.slice(1), 16);
  return [(n >> 16) / 255, ((n >> 8) & 255) / 255, (n & 255) / 255];
}

// Same camera as PoseViewer.project(): rotate by yaw then pitch, orthographic scale.
// project() returns pixel x, pixel y and a depth where bigger means farther away.
const CAMERA_GLSL = `
uniform vec3 uCenter;
uniform vec2 uYaw;
uniform vec2 uPitch;
uniform vec2 uSize;
uniform float uScale;
uniform float uDepthScale;
vec3 project(vec3 p) {
  vec3 d = p - uCenter;
  float tx = d.x * uYaw.x + d.z * uYaw.y;
  float tz = -d.x * uYaw.y + d.z * uYaw.x;
  float yy = d.y * uPitch.x - tz * uPitch.y;
  float zz = d.y * uPitch.y + tz * uPitch.x;
  return vec3(uSize.x * 0.5 + tx * uScale, uSize.y * 0.49 - yy * uScale, zz);
}
vec4 toClip(vec3 s) {
  return vec4(s.x / uSize.x * 2.0 - 1.0, 1.0 - s.y / uSize.y * 2.0, s.z * uDepthScale, 1.0);
}
`;

// Mesh: one flat-shaded gray per triangle, computed on the CPU (aPosShade.w).
const MESH_VERTEX = `${CAMERA_GLSL}
attribute vec4 aPosShade;
varying float vShade;
void main() {
  vShade = aPosShade.w;
  gl_Position = toClip(project(aPosShade.xyz));
}`;
const MESH_FRAGMENT = `
precision mediump float;
varying float vShade;
void main() {
  gl_FragColor = vec4(vec3(vShade), 1.0);
}`;

// Segments: floor lines, bones and joints. Each one is a quad around the screen-space
// segment A-B, and the fragment shader draws a rounded line with an optional outline.
// A joint is a segment whose two ends are the same point, which gives a disc.
const SEGMENT_VERTEX = `${CAMERA_GLSL}
attribute vec3 aA;
attribute vec3 aB;
attribute vec2 aCorner; // x: 0 at A, 1 at B. y: -1 or +1 side of the line.
uniform vec3 uOrigin;
uniform float uUnit;
uniform float uWidth;
uniform float uOutline;
varying vec2 vLocal;
varying float vLen;
void main() {
  vec3 a = project(uOrigin + uUnit * aA);
  vec3 b = project(uOrigin + uUnit * aB);
  vec2 delta = b.xy - a.xy;
  float len = length(delta);
  vec2 dir = len > 0.0001 ? delta / len : vec2(1.0, 0.0);
  vec2 side = vec2(-dir.y, dir.x);
  float reach = (uWidth + uOutline) * 0.5 + 1.0;
  float along = aCorner.x * len + (aCorner.x * 2.0 - 1.0) * reach;
  vec2 pixel = a.xy + dir * along + side * aCorner.y * reach;
  vLocal = vec2(along, aCorner.y * reach);
  vLen = len;
  gl_Position = toClip(vec3(pixel, mix(a.z, b.z, aCorner.x)));
}`;
const SEGMENT_FRAGMENT = `
#ifdef GL_FRAGMENT_PRECISION_HIGH
precision highp float;
#else
precision mediump float;
#endif
uniform float uWidth;
uniform float uOutline;
uniform vec3 uColor0;
uniform vec3 uColor1;
uniform vec3 uOutlineColor;
varying vec2 vLocal;
varying float vLen;
void main() {
  float along = clamp(vLocal.x, 0.0, vLen);
  float dist = length(vec2(vLocal.x - along, vLocal.y));
  float outer = clamp((uWidth + uOutline) * 0.5 + 0.5 - dist, 0.0, 1.0);
  float inner = clamp(uWidth * 0.5 + 0.5 - dist, 0.0, 1.0);
  if (outer <= 0.0) discard;
  vec3 fill = mix(uColor0, uColor1, along / max(vLen, 0.0001));
  vec3 color = mix(uOutlineColor, fill, inner);
  gl_FragColor = vec4(color * outer, outer);
}`;

const CAMERA_UNIFORMS = ['uCenter', 'uYaw', 'uPitch', 'uSize', 'uScale', 'uDepthScale'];
const SEGMENT_UNIFORMS = [
  ...CAMERA_UNIFORMS, 'uOrigin', 'uUnit', 'uWidth', 'uOutline', 'uColor0', 'uColor1', 'uOutlineColor',
];

function buildProgram(gl, vertexSource, fragmentSource, attributes, uniformNames) {
  const compile = (type, source) => {
    const shader = gl.createShader(type);
    gl.shaderSource(shader, source);
    gl.compileShader(shader);
    if (!gl.getShaderParameter(shader, gl.COMPILE_STATUS)) throw new Error(gl.getShaderInfoLog(shader));
    return shader;
  };
  const program = gl.createProgram();
  gl.attachShader(program, compile(gl.VERTEX_SHADER, vertexSource));
  gl.attachShader(program, compile(gl.FRAGMENT_SHADER, fragmentSource));
  attributes.forEach((name, index) => gl.bindAttribLocation(program, index, name));
  gl.linkProgram(program);
  if (!gl.getProgramParameter(program, gl.LINK_STATUS)) throw new Error(gl.getProgramInfoLog(program));
  const uniforms = {};
  for (const name of uniformNames) uniforms[name] = gl.getUniformLocation(program, name);
  return { program, uniforms, attributeCount: attributes.length };
}

// Fills `out` with one flat-shaded triangle per mesh triangle (3 vertices of x, y, z, shade).
// Returns the number of vertices written.
function fillMesh(out, vertices, corners) {
  let at = 0;
  for (let t = 0; t < corners.length; t += 3) {
    const a = vertices[corners[t]],
      b = vertices[corners[t + 1]],
      c = vertices[corners[t + 2]];
    if (!a || !b || !c) continue;
    const ux = b[0] - a[0], uy = b[1] - a[1], uz = b[2] - a[2];
    const vx = c[0] - a[0], vy = c[1] - a[1], vz = c[2] - a[2];
    const nx = uy * vz - uz * vy, ny = uz * vx - ux * vz, nz = ux * vy - uy * vx;
    const len = Math.hypot(nx, ny, nz) || 1;
    // Same light direction and brightness range as the Canvas 2D path.
    const shade = Math.round(105 + 100 * Math.abs((nx * 0.25 + ny * 0.8 + nz * 0.54) / len)) / 255;
    for (const p of [a, b, c]) {
      out[at++] = p[0];
      out[at++] = p[1];
      out[at++] = p[2];
      out[at++] = shade;
    }
  }
  return at / 4;
}

// Writes the 4 corner vertices of one segment quad as (A, B) pairs: 24 floats.
function writeQuad(out, at, a, b) {
  for (let corner = 0; corner < 4; corner++) {
    out[at++] = a[0]; out[at++] = a[1]; out[at++] = a[2];
    out[at++] = b[0]; out[at++] = b[1]; out[at++] = b[2];
  }
  return at;
}

class GpuRenderer {
  constructor(canvas, gl, backend) {
    this.canvas = canvas;
    this.gl = gl;
    this.backend = backend; // 'webgl2' or 'webgl'
    this.lost = false;
    this.broken = false;
    this.scratch = new Float32Array(0);
    // preventDefault tells the browser we want the context back.
    canvas.addEventListener('webglcontextlost', (e) => {
      e.preventDefault();
      this.lost = true;
    });
    canvas.addEventListener('webglcontextrestored', () => {
      try {
        this.setup();
        this.lost = false;
      } catch (error) {
        this.broken = true;
      }
    });
    this.setup();
  }
  // Creates every GL object. Runs again after a restored context, because those objects are gone.
  setup() {
    const gl = this.gl;
    this.meshProgram = buildProgram(gl, MESH_VERTEX, MESH_FRAGMENT, ['aPosShade'], CAMERA_UNIFORMS);
    this.segmentProgram = buildProgram(
      gl, SEGMENT_VERTEX, SEGMENT_FRAGMENT, ['aA', 'aB', 'aCorner'], SEGMENT_UNIFORMS,
    );
    this.dynamicBuffer = gl.createBuffer(); // refilled for every drawn frame
    this.quadCache = new Map(); // static quad buffers, keyed by quad count
    this.maxSize = gl.getParameter(gl.MAX_VIEWPORT_DIMS)?.[0] || 2048;
    this.setupFloor();
  }
  // The floor never changes in grid units, so it is uploaded once. The shader places it with
  // uOrigin and uUnit. The two center lines come last so they can use a different color.
  setupFloor() {
    const gl = this.gl, n = FLOOR_HALF_LINES;
    const lines = [], centerLines = [];
    for (let i = -n; i <= n; i++) {
      const target = i === 0 ? centerLines : lines;
      target.push([[i, 0, -n], [i, 0, n]], [[-n, 0, i], [n, 0, i]]);
    }
    const all = [...lines, ...centerLines];
    const data = new Float32Array(all.length * 24);
    all.forEach(([a, b], i) => writeQuad(data, i * 24, a, b));
    this.floorBuffer = gl.createBuffer();
    gl.bindBuffer(gl.ARRAY_BUFFER, this.floorBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, data, gl.STATIC_DRAW);
    this.floorQuads = this.quadsFor(all.length);
    this.floorLineCount = lines.length;
  }
  get usable() {
    return !this.broken && !this.lost && !this.gl.isContextLost();
  }
  // Corner coordinates and indices for `count` quads. They only depend on the count,
  // so every clip with the same number of bones shares one upload.
  quadsFor(count) {
    if (count > MAX_QUADS) return null;
    let quads = this.quadCache.get(count);
    if (quads) return quads;
    const gl = this.gl;
    const corners = new Float32Array(count * 8);
    const indices = new Uint16Array(count * 6);
    for (let q = 0; q < count; q++) {
      corners.set([0, -1, 0, 1, 1, -1, 1, 1], q * 8);
      indices.set([0, 1, 2, 2, 1, 3].map((v) => v + q * 4), q * 6);
    }
    quads = { corners: gl.createBuffer(), indices: gl.createBuffer() };
    gl.bindBuffer(gl.ARRAY_BUFFER, quads.corners);
    gl.bufferData(gl.ARRAY_BUFFER, corners, gl.STATIC_DRAW);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, quads.indices);
    gl.bufferData(gl.ELEMENT_ARRAY_BUFFER, indices, gl.STATIC_DRAW);
    this.quadCache.set(count, quads);
    return quads;
  }
  scratchArray(length) {
    if (this.scratch.length < length) this.scratch = new Float32Array(length);
    return this.scratch;
  }
  // Copies `values` into the shared dynamic vertex buffer and leaves it bound.
  upload(values) {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, this.dynamicBuffer);
    gl.bufferData(gl.ARRAY_BUFFER, values, gl.DYNAMIC_DRAW);
  }
  useProgram(program, scene) {
    const gl = this.gl;
    gl.useProgram(program.program);
    for (let i = 0; i < 3; i++) {
      if (i < program.attributeCount) gl.enableVertexAttribArray(i);
      else gl.disableVertexAttribArray(i);
    }
    const u = program.uniforms, cam = scene.camera;
    gl.uniform3f(u.uCenter, cam.center[0], cam.center[1], cam.center[2]);
    gl.uniform2f(u.uYaw, cam.cosYaw, cam.sinYaw);
    gl.uniform2f(u.uPitch, cam.cosPitch, cam.sinPitch);
    gl.uniform2f(u.uSize, scene.bitmapWidth, scene.bitmapHeight);
    gl.uniform1f(u.uScale, cam.scale * scene.dpr);
    gl.uniform1f(u.uDepthScale, 1 / (scene.span * 20));
  }
  // Selects the segment shader and sets how thick/colored the next segments are.
  useSegments(scene, { origin = [0, 0, 0], unit = 1, width, outline = 0, color0, color1 = color0 }) {
    const gl = this.gl, program = this.segmentProgram, u = program.uniforms;
    this.useProgram(program, scene);
    gl.uniform3f(u.uOrigin, origin[0], origin[1], origin[2]);
    gl.uniform1f(u.uUnit, unit);
    gl.uniform1f(u.uWidth, width * scene.dpr);
    gl.uniform1f(u.uOutline, outline * scene.dpr);
    gl.uniform3f(u.uColor0, ...rgb(color0));
    gl.uniform3f(u.uColor1, ...rgb(color1));
    gl.uniform3f(u.uOutlineColor, ...rgb(COLORS.boneOutline));
  }
  // Points the three segment attributes at a vertex buffer and a set of static quads.
  bindSegments(vertexBuffer, quads) {
    const gl = this.gl;
    gl.bindBuffer(gl.ARRAY_BUFFER, vertexBuffer);
    gl.vertexAttribPointer(0, 3, gl.FLOAT, false, 24, 0);
    gl.vertexAttribPointer(1, 3, gl.FLOAT, false, 24, 12);
    gl.bindBuffer(gl.ARRAY_BUFFER, quads.corners);
    gl.vertexAttribPointer(2, 2, gl.FLOAT, false, 8, 0);
    gl.bindBuffer(gl.ELEMENT_ARRAY_BUFFER, quads.indices);
  }
  drawQuads(first, count) {
    const gl = this.gl;
    gl.drawElements(gl.TRIANGLES, count * 6, gl.UNSIGNED_SHORT, first * 12);
  }
  drawFloor(scene) {
    const center = scene.camera.center, unit = scene.span / 5;
    const origin = [center[0], scene.base, center[2]];
    this.useSegments(scene, { origin, unit, width: 1, color0: COLORS.gridLine });
    this.bindSegments(this.floorBuffer, this.floorQuads);
    this.drawQuads(0, this.floorLineCount);
    this.useSegments(scene, { origin, unit, width: 1, color0: COLORS.gridCenter });
    this.drawQuads(this.floorLineCount, 2);
  }
  drawMesh(scene) {
    const gl = this.gl;
    const out = this.scratchArray(scene.corners.length * 4);
    const count = fillMesh(out, scene.vertices, scene.corners);
    if (!count) return;
    this.useProgram(this.meshProgram, scene);
    this.upload(out.subarray(0, count * 4));
    gl.vertexAttribPointer(0, 4, gl.FLOAT, false, 16, 0);
    gl.enable(gl.DEPTH_TEST);
    gl.drawArrays(gl.TRIANGLES, 0, count);
    gl.disable(gl.DEPTH_TEST);
  }
  drawBones(scene, quads) {
    const gl = this.gl, f = scene.points;
    const out = this.scratchArray(scene.segments.length * 24);
    let at = 0, count = 0;
    for (const bone of scene.segments) {
      if (!f[bone.i] || !f[bone.parent]) continue;
      at = writeQuad(out, at, f[bone.parent], f[bone.i]);
      count++;
    }
    if (!count) return;
    const width = Math.max(3, Math.min(13, scene.span * 0.022 * scene.camera.scale));
    this.useSegments(scene, {
      width, outline: 3, color0: COLORS.boneStart, color1: COLORS.boneEnd,
    });
    this.upload(out.subarray(0, at));
    this.bindSegments(this.dynamicBuffer, quads);
    // Bones are drawn over the mesh, but sort among themselves with the depth buffer.
    gl.clear(gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.DEPTH_TEST);
    this.drawQuads(0, count);
    gl.disable(gl.DEPTH_TEST);
  }
  drawJoints(scene, quads) {
    const f = scene.points;
    const out = this.scratchArray(f.length * 24);
    let at = 0;
    for (const point of f) at = writeQuad(out, at, point, point);
    const width = 2 * Math.max(2, Math.min(5, scene.span * 0.009 * scene.camera.scale));
    // Joint 0 (the root) is white, drawn first; the other joints share one color.
    this.useSegments(scene, { width, color0: COLORS.rootJoint });
    this.upload(out.subarray(0, at));
    this.bindSegments(this.dynamicBuffer, quads);
    this.drawQuads(0, 1);
    if (f.length > 1) {
      this.useSegments(scene, { width, color0: COLORS.joint });
      this.drawQuads(1, f.length - 1);
    }
  }
  // Renders one viewer's frame in the shared canvas, then copies it onto `target`
  // (the viewer's 2D context). Returns false when the caller must draw with Canvas 2D.
  render(target, scene) {
    if (!this.usable) return false;
    const gl = this.gl, { bitmapWidth: bw, bitmapHeight: bh } = scene;
    if (bw > this.maxSize || bh > this.maxSize) return false;
    const drawBones = scene.skeleton && scene.points?.length > 0;
    const boneQuads = drawBones ? this.quadsFor(scene.segments.length || 1) : null;
    const jointQuads = drawBones ? this.quadsFor(scene.points.length) : null;
    if (drawBones && (!boneQuads || !jointQuads)) return false;
    // The shared canvas only grows. Each viewer uses the bottom-left bw x bh corner of it.
    if (this.canvas.width < bw) this.canvas.width = bw;
    if (this.canvas.height < bh) this.canvas.height = bh;
    gl.viewport(0, 0, bw, bh);
    gl.enable(gl.SCISSOR_TEST);
    gl.scissor(0, 0, bw, bh);
    gl.clearColor(0, 0, 0, 0);
    gl.clear(gl.COLOR_BUFFER_BIT | gl.DEPTH_BUFFER_BIT);
    gl.enable(gl.BLEND);
    gl.blendFunc(gl.ONE, gl.ONE_MINUS_SRC_ALPHA); // the canvas holds premultiplied colors
    gl.depthFunc(gl.LESS);
    gl.disable(gl.DEPTH_TEST);
    if (scene.floor) this.drawFloor(scene);
    if (scene.vertices && scene.corners) this.drawMesh(scene);
    if (drawBones) {
      this.drawBones(scene, boneQuads);
      this.drawJoints(scene, jointQuads);
    }
    // WebGL's origin is bottom-left, so our corner is the bottom rows of the canvas image.
    target.drawImage(this.canvas, 0, this.canvas.height - bh, bw, bh, 0, 0, scene.width, scene.height);
    return true;
  }
}

function createRenderer() {
  try {
    if (typeof document === 'undefined') return null;
    // Never attached to the page: nobody sees this canvas, it only feeds drawImage.
    const canvas = document.createElement('canvas');
    const options = { alpha: true, antialias: true, depth: true, premultipliedAlpha: true };
    for (const backend of ['webgl2', 'webgl']) {
      const gl = canvas.getContext(backend, options);
      if (gl) return new GpuRenderer(canvas, gl, backend);
    }
  } catch (error) {
    // Fall through: the viewers keep working with Canvas 2D.
  }
  return null;
}

let sharedRenderer; // undefined = not tried yet, null = WebGL is not available

// The shared renderer, or null while WebGL cannot be used (viewers then draw with Canvas 2D).
function gpuRenderer() {
  if (sharedRenderer === undefined) sharedRenderer = createRenderer();
  return sharedRenderer?.usable ? sharedRenderer : null;
}

// Which drawing path viewers use right now: 'webgl2', 'webgl' or 'canvas2d'. For diagnostics.
export function viewerBackend() {
  return gpuRenderer()?.backend ?? 'canvas2d';
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
    this.paintBackground(g, w, h);
    if (!this.drawWithGpu(g, w, h, dpr)) this.drawWithCanvas(g, w, h);
    this.lastDrawn = signature;
  }
  paintBackground(g, w, h) {
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
  }
  // Draws floor, mesh and skeleton on the GPU and copies them over the background.
  // Returns false if that is not possible, so the caller can use Canvas 2D instead.
  drawWithGpu(g, w, h, dpr) {
    const renderer = gpuRenderer();
    if (!renderer) return false;
    const mesh = this.data.mesh,
      frames = this.data.frames;
    const vertices = mesh?.frames?.[Math.min(this.frame, mesh.frames.length - 1)];
    const cos = Math.cos, sin = Math.sin;
    const scene = {
      width: w,
      height: h,
      dpr,
      bitmapWidth: Math.round(w * dpr),
      bitmapHeight: Math.round(h * dpr),
      camera: {
        center: this.center,
        cosYaw: cos(this.yaw),
        sinYaw: sin(this.yaw),
        cosPitch: cos(this.pitch),
        sinPitch: sin(this.pitch),
        scale: (Math.min(w, h) * 0.68 * this.zoom) / this.span, // CSS pixels per unit
      },
      span: this.span,
      base: this.base,
      floor: this.floor,
      skeleton: this.skeleton,
      vertices,
      corners: vertices && mesh.triangles ? meshCorners(mesh.triangles) : null,
      points: frames[Math.min(this.frame, frames.length - 1)],
      segments: this.segments,
    };
    try {
      return renderer.render(g, scene);
    } catch (error) {
      // A GL error should never reach the app: stop using the GPU and draw with Canvas 2D.
      console.warn('WebGL preview failed, using Canvas 2D', error);
      renderer.broken = true;
      return false;
    }
  }
  // The Canvas 2D drawing: used when WebGL is unavailable or lost.
  drawWithCanvas(g, w, h) {
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
      const n = FLOOR_HALF_LINES,
        step = this.span / 5;
      for (let i = -n; i <= n; i++) {
        const color = i === 0 ? COLORS.gridCenter : COLORS.gridLine;
        line(
          [this.center[0] + i * step, this.base, this.center[2] - n * step],
          [this.center[0] + i * step, this.base, this.center[2] + n * step],
          color,
        );
        line(
          [this.center[0] - n * step, this.base, this.center[2] + i * step],
          [this.center[0] + n * step, this.base, this.center[2] + i * step],
          color,
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
    if (!f || !this.skeleton) return;
    g.lineCap = 'round';
    const projected = f.map((point) => this.project(point, w, h));
    const segments = this.segments.filter((bone) => f[bone.i] && f[bone.parent]);
    segments.sort((a, b) => projected[b.i][2] - projected[a.i][2]);
    for (const b of segments) {
      const a = projected[b.parent],
        v = projected[b.i];
      const width = Math.max(3, Math.min(13, this.span * 0.022 * a[3]));
      g.strokeStyle = COLORS.boneOutline;
      g.lineWidth = width + 3;
      g.beginPath();
      g.moveTo(a[0], a[1]);
      g.lineTo(v[0], v[1]);
      g.stroke();
      const grad = g.createLinearGradient(a[0], a[1], v[0], v[1]);
      grad.addColorStop(0, COLORS.boneStart);
      grad.addColorStop(1, COLORS.boneEnd);
      g.strokeStyle = grad;
      g.lineWidth = width;
      g.stroke();
    }
    projected.forEach((v, i) => {
      g.beginPath();
      g.arc(v[0], v[1], Math.max(2, Math.min(5, this.span * 0.009 * v[3])), 0, Math.PI * 2);
      g.fillStyle = i === 0 ? COLORS.rootJoint : COLORS.joint;
      g.fill();
    });
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
  // Viewers own no GPU objects (the shared renderer does), so this only detaches listeners.
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
