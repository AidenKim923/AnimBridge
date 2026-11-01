import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

// Import the browser ES module without changing the project's module format.
const source = await readFile(process.argv[2], 'utf8');
// Each tag gives a fresh module instance, so every instance starts without a shared WebGL renderer.
const loadViewer = (tag = '') =>
  import(`data:text/javascript;base64,${Buffer.from(`${source}\n// ${tag}`).toString('base64')}`);
const { PoseViewer, viewerBackend } = await loadViewer();
globalThis.devicePixelRatio = 1;
globalThis.ResizeObserver = class {
  constructor(callback) { this.callback = callback; }
  observe() {}
  disconnect() { this.disconnected = true; }
};

function canvas() {
  // `calls` records every 2D method name used; `images` holds drawImage arguments.
  const stats = { draws: 0, allocations: 0, transforms: [], contexts: [], calls: new Set(), images: [] };
  const gradient = () => ({ addColorStop() {} });
  const ctx = new Proxy({
    setTransform(...args) { stats.draws++; stats.transforms.push(args); },
    drawImage(...args) { stats.images.push(args); },
    createRadialGradient: gradient,
    createLinearGradient: gradient,
  }, {
    get(target, key) {
      stats.calls.add(key);
      return key in target ? target[key] : () => {};
    },
  });
  const result = {
    clientWidth: 240, clientHeight: 180, stats,
    listeners: new Map(),
    getContext: (type) => { stats.contexts.push(type); return ctx; },
    addEventListener(name, handler) { this.listeners.set(name, handler); },
    removeEventListener(name) { this.listeners.delete(name); },
    setPointerCapture() {},
  };
  for (const key of ['width', 'height']) {
    let value = 0;
    Object.defineProperty(result, key, {
      get: () => value,
      set(next) { value = next; stats.allocations++; },
    });
  }
  return result;
}

let visits = 0;
function trackedFrames(frames) {
  return new Proxy(frames, {
    get(target, key, receiver) {
      if (key === Symbol.iterator) return function* () {
        for (const frame of target) { visits++; yield frame; }
      };
      return Reflect.get(target, key, receiver);
    },
  });
}
const data = {
  bones: [{ name: 'Root', parent: -1 }, { name: 'Head', parent: 'Root' }],
  frames: trackedFrames([[[0, 0, 0], [0, 2, 0]], [[2, 0, 0], [2, 2, 0]]]),
  mesh: { triangles: [[0, 1, 2]], frames: [
    [[0, 0, 0], [1, 0, 0], [0, 1, 0]],
    [[1, 0, 0], [2, 0, 0], [1, 1, 0]],
  ] },
};

// Every card/detail instance uses one shared bounds/parent computation.
const hiddenCanvas = canvas();
const hidden = new PoseViewer(hiddenCanvas, data, { visible: false });
const other = new PoseViewer(canvas(), data, { visible: false });
assert.equal(visits, 2);
assert.equal(hidden.segments, other.segments);
assert.deepEqual(hidden.segments, [{ i: 1, parent: 0 }]);
assert.deepEqual(hidden.center, [1, 1, 0]);
assert.equal(hidden.span, 2);
hidden.center[0] = 100;
assert.equal(other.center[0], 1, 'camera centers must not be shared mutable arrays');
assert.equal(hiddenCanvas.stats.draws, 0);
data.frames = trackedFrames([[[0, 0, 0], [0, 4, 0]]]);
const replaced = new PoseViewer(canvas(), data, { visible: false });
assert.equal(visits, 3, 'replacing frames invalidates cached bounds');
assert.equal(replaced.span, 4);

// A live animation repaints when needed without reallocating the canvas bitmap.
const motion = { ...data, frames: [[[0, 0, 0], [0, 2, 0]], [[2, 0, 0], [2, 2, 0]]] };
const c = canvas();
const viewer = new PoseViewer(c, motion);
assert.equal(c.stats.draws, 1);
assert.equal(c.stats.allocations, 2);
viewer.setFrame(0);
assert.equal(c.stats.draws, 1, 'same frame and options skip drawing');
viewer.setFrame(1);
assert.equal(c.stats.draws, 2);
assert.equal(c.stats.allocations, 2, 'frame changes retain the canvas backing store');
viewer.floor = false;
viewer.setFrame(1);
assert.equal(c.stats.draws, 3, 'floor changes repaint even at the same frame');
viewer.skeleton = false;
viewer.setFrame(1);
assert.equal(c.stats.draws, 4, 'bone visibility changes repaint even at the same frame');
viewer.yaw += 0.1;
viewer.draw();
assert.equal(c.stats.draws, 5, 'camera changes repaint');
viewer.reset();
assert.equal(c.stats.draws, 6);
viewer.setVisible(false);
viewer.setFrame(0);
assert.equal(c.stats.draws, 6, 'offscreen animation updates do not paint');
viewer.setVisible(true);
assert.equal(c.stats.draws, 7, 'visible again paints the current frame');
c.clientWidth = 300;
viewer.observer.callback();
assert.equal(c.stats.draws, 8);
assert.equal(c.stats.allocations, 3, 'resize only changes the affected dimension');
globalThis.devicePixelRatio = 2;
viewer.draw();
assert.equal(c.stats.draws, 9, 'a new device pixel ratio repaints');
assert.deepEqual(c.stats.transforms.at(-1), [2, 0, 0, 2, 0, 0]);
viewer.setFrame(-100);
assert.equal(viewer.frame, 0);
viewer.setFrame(100);
assert.equal(viewer.frame, 1);
viewer.setFrame(NaN);
assert.equal(viewer.frame, 0);
viewer.destroy();
assert.equal(viewer.observer.disconnected, true);
assert.equal(c.listeners.size, 0);
assert.equal(viewerBackend(), 'canvas2d', 'Node has no WebGL, so Canvas 2D is chosen automatically');
console.log('PoseViewer cache, redraw, visibility and resize checks passed');

// ---- WebGL path, using a fake context that records the calls it receives ----

globalThis.devicePixelRatio = 1;

// Only the GL methods viewer.js uses exist; any other call fails the test.
function fakeGl() {
  const calls = [];
  const state = { lost: false, throwOnDraw: false };
  const record = (name, result) => (...args) => { calls.push([name, ...args]); return result; };
  const methods = {
    createShader: {}, createProgram: {}, createBuffer: {}, getUniformLocation: {},
    getShaderParameter: true, getProgramParameter: true,
    getParameter: [4096, 4096],
    shaderSource: undefined, compileShader: undefined, attachShader: undefined,
    bindAttribLocation: undefined, linkProgram: undefined, useProgram: undefined,
    bindBuffer: undefined, bufferData: undefined, enableVertexAttribArray: undefined,
    disableVertexAttribArray: undefined, vertexAttribPointer: undefined,
    uniform1f: undefined, uniform2f: undefined, uniform3f: undefined,
    enable: undefined, disable: undefined, blendFunc: undefined, depthFunc: undefined,
    viewport: undefined, scissor: undefined, clearColor: undefined, clear: undefined,
    drawArrays: undefined,
  };
  return new Proxy({}, {
    get(target, key) {
      if (key === 'calls') return calls;
      if (key === 'state') return state;
      if (key === 'isContextLost') return () => state.lost;
      if (key === 'drawElements') {
        return (...args) => {
          if (state.throwOnDraw) throw new Error('GL_INVALID_OPERATION');
          calls.push(['drawElements', ...args]);
        };
      }
      if (key in methods) return record(key, methods[key]);
      // GL constants such as gl.DEPTH_TEST: any stable number works.
      if (typeof key === 'string' && /^[A-Z][A-Z0-9_]+$/.test(key)) {
        return [...key].reduce((sum, ch) => (sum * 31 + ch.charCodeAt(0)) % 100003, 7);
      }
      throw new Error(`fake GL has no ${String(key)}`);
    },
  });
}

// A hidden canvas as document.createElement('canvas') would make it.
function fakeGlCanvas(supported) {
  const gl = fakeGl();
  const listeners = new Map();
  return {
    gl, contexts: [], width: 1, height: 1,
    getContext(type) { this.contexts.push(type); return supported.includes(type) ? gl : null; },
    addEventListener(name, handler) { listeners.set(name, handler); },
    dispatch(name) {
      const event = { prevented: false, preventDefault() { this.prevented = true; } };
      listeners.get(name)(event);
      return event;
    },
  };
}

function withDocument(glCanvas) {
  const created = [];
  globalThis.document = { createElement: (tag) => { created.push(tag); return glCanvas; } };
  return created;
}

const staticUploads = (gl) =>
  gl.calls.filter(([name, , , usage]) => name === 'bufferData' && usage === gl.STATIC_DRAW).length;
const drawCount = (gl) => gl.calls.filter(([name]) => name.startsWith('draw')).length;
const variant = (extra = {}) => ({ ...motion, ...extra });

{
  // One shared WebGL2 context serves every viewer.
  const glCanvas = fakeGlCanvas(['webgl2', 'webgl']);
  const created = withDocument(glCanvas);
  const module = await loadViewer('shared');
  assert.equal(created.length, 0, 'no canvas is created before the first viewer draws');
  const first = canvas(), second = canvas(), third = canvas();
  const a = new module.PoseViewer(first, motion);
  const b = new module.PoseViewer(second, motion);
  const longer = variant({
    bones: [...motion.bones, { name: 'Tip', parent: 'Head' }],
    frames: motion.frames.map((f) => [...f, [0, 3, 0]]),
  });
  const c = new module.PoseViewer(third, longer);
  assert.equal(module.viewerBackend(), 'webgl2');
  assert.deepEqual(created, ['canvas'], 'one hidden canvas for all viewers');
  assert.deepEqual(glCanvas.contexts, ['webgl2'], 'one WebGL context for all viewers');
  for (const card of [first, second, third]) {
    assert.deepEqual(card.stats.contexts, ['2d'], 'card canvases never request WebGL');
    assert.equal(card.stats.images.length, 1, 'each card receives its frame through drawImage');
    assert.ok(!card.stats.calls.has('moveTo'), 'no Canvas 2D geometry is drawn on the GPU path');
  }
  // Source rectangle is the bottom-left corner of the GL canvas; destination is the card.
  assert.deepEqual(first.stats.images[0].slice(1), [0, 0, 240, 180, 0, 0, 240, 180]);
  assert.ok(glCanvas.width >= 240 && glCanvas.height >= 180);
  const gl = glCanvas.gl;
  assert.ok(gl.calls.some(([name, flag]) => name === 'enable' && flag === gl.DEPTH_TEST),
    'the depth buffer sorts the mesh');
  assert.ok(gl.calls.some(([name]) => name === 'drawArrays'), 'mesh triangles are drawn');

  // Static buffers are uploaded once, however many frames or viewers draw.
  const uploads = staticUploads(gl);
  const draws = drawCount(gl);
  for (let frame = 0; frame < 6; frame++) {
    a.setFrame(frame % 2);
    b.setFrame((frame + 1) % 2);
    c.setFrame(frame % 2);
    a.yaw += 0.1;
    a.draw();
  }
  assert.ok(drawCount(gl) > draws, 'animation frames are drawn on the GPU');
  assert.equal(staticUploads(gl), uploads, 'static buffers are not uploaded again per frame');
  const sameSize = new module.PoseViewer(canvas(), variant());
  assert.equal(staticUploads(gl), uploads, 'clips with the same bone count reuse static buffers');
  assert.equal(glCanvas.contexts.length, 1);

  // A hidden viewer does not draw; destroying viewers leaves the shared context alone.
  const before = drawCount(gl);
  sameSize.setVisible(false);
  sameSize.setFrame(1);
  assert.equal(drawCount(gl), before);
  [a, b, c, sameSize].forEach((v) => v.destroy());
  assert.deepEqual(created, ['canvas']);
  assert.ok(!gl.calls.some(([name]) => name.startsWith('delete')));

  // Context loss: viewers draw with Canvas 2D without throwing, then use the GPU again.
  const card = canvas();
  const viewer = new module.PoseViewer(card, motion);
  assert.equal(card.stats.images.length, 1);
  gl.state.lost = true;
  const lost = glCanvas.dispatch('webglcontextlost');
  assert.equal(lost.prevented, true, 'the browser is told we want the context back');
  assert.equal(module.viewerBackend(), 'canvas2d');
  viewer.setFrame(1);
  assert.equal(card.stats.images.length, 1, 'lost context: nothing is copied from the GL canvas');
  assert.ok(card.stats.calls.has('moveTo'), 'lost context: Canvas 2D drawing takes over');
  const lateCard = canvas();
  new module.PoseViewer(lateCard, motion);
  assert.ok(lateCard.stats.calls.has('moveTo'), 'viewers created during the loss still draw');
  gl.state.lost = false;
  glCanvas.dispatch('webglcontextrestored');
  assert.equal(module.viewerBackend(), 'webgl2');
  viewer.setFrame(0);
  assert.equal(card.stats.images.length, 2, 'restored context: the GPU path is used again');
  assert.equal(glCanvas.contexts.length, 1, 'the same context is reused after restoring');

  // A GL error falls back to Canvas 2D instead of reaching the app.
  gl.state.throwOnDraw = true;
  const originalWarn = console.warn;
  console.warn = () => {};
  viewer.setFrame(1);
  console.warn = originalWarn;
  assert.equal(card.stats.images.length, 2);
  assert.equal(module.viewerBackend(), 'canvas2d');
  viewer.destroy();
}

{
  // WebGL1 is used when WebGL2 is missing.
  const glCanvas = fakeGlCanvas(['webgl']);
  withDocument(glCanvas);
  const module = await loadViewer('webgl1');
  const card = canvas();
  new module.PoseViewer(card, motion);
  assert.deepEqual(glCanvas.contexts, ['webgl2', 'webgl']);
  assert.equal(module.viewerBackend(), 'webgl');
  assert.equal(card.stats.images.length, 1);
}

{
  // No WebGL at all, or a failing document: Canvas 2D without errors, and no retry per viewer.
  const glCanvas = fakeGlCanvas([]);
  withDocument(glCanvas);
  const module = await loadViewer('none');
  const card = canvas();
  new module.PoseViewer(card, motion);
  new module.PoseViewer(canvas(), motion);
  assert.equal(module.viewerBackend(), 'canvas2d');
  assert.ok(card.stats.calls.has('moveTo'));
  assert.deepEqual(glCanvas.contexts, ['webgl2', 'webgl'], 'creation is attempted only once');
  globalThis.document = { createElement() { throw new Error('no DOM'); } };
  const broken = await loadViewer('throws');
  new broken.PoseViewer(canvas(), motion);
  assert.equal(broken.viewerBackend(), 'canvas2d');
}
delete globalThis.document;
console.log('PoseViewer WebGL sharing, static buffer reuse and context loss checks passed');
