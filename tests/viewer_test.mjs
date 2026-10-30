import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

// Import the browser ES module without changing the project's module format.
const source = await readFile(process.argv[2], 'utf8');
const { PoseViewer } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);
globalThis.devicePixelRatio = 1;
globalThis.ResizeObserver = class {
  constructor(callback) { this.callback = callback; }
  observe() {}
  disconnect() { this.disconnected = true; }
};

function canvas() {
  const stats = { draws: 0, allocations: 0, transforms: [] };
  const gradient = () => ({ addColorStop() {} });
  const ctx = new Proxy({
    setTransform(...args) { stats.draws++; stats.transforms.push(args); },
    createRadialGradient: gradient,
    createLinearGradient: gradient,
  }, { get(target, key) { return key in target ? target[key] : () => {}; } });
  const result = {
    clientWidth: 240, clientHeight: 180, stats,
    listeners: new Map(),
    getContext: () => ctx,
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
console.log('PoseViewer cache, redraw, visibility and resize checks passed');
