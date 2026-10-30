import assert from 'node:assert/strict';
import { readFile } from 'node:fs/promises';

const source = await readFile(process.argv[2], 'utf8');
const { PreviewQueue } = await import(`data:text/javascript;base64,${Buffer.from(source).toString('base64')}`);

// A deterministic timer keeps the retry/race checks fast and reproducible.
let nextTimer = 0;
const timers = new Map();
globalThis.setTimeout = (callback, delay = 0) => {
  const id = ++nextTimer;
  timers.set(id, { callback, delay });
  return id;
};
globalThis.clearTimeout = (id) => timers.delete(id);
async function settle() {
  for (let i = 0; i < 20; i++) await Promise.resolve();
}
async function fire() {
  assert(timers.size, 'expected a scheduled queue pump');
  const [id, timer] = timers.entries().next().value;
  timers.delete(id);
  timer.callback();
  await settle();
}
function defer() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}
function clip(id, version = 1) { return { id, version }; }
const keyFor = (item) => `${item.id}:${item.version}`;

function harness(options = {}) {
  timers.clear();
  const cache = new Map(), requests = [], changed = [];
  let active = 0, maximum = 0;
  const queue = new PreviewQueue({
    cache,
    changed: (id) => changed.push(id),
    load: (item) => {
      const completion = defer();
      requests.push({ item, ...completion });
      maximum = Math.max(maximum, ++active);
      return completion.promise.finally(() => { active--; });
    },
    ...options,
  });
  return { queue, cache, requests, changed, maximum: () => maximum };
}

{
  const { queue, cache, requests, maximum } = harness();
  const clips = Array.from({ length: 7 }, (_, i) => clip(`clip-${i}`));
  queue.sync(clips, keyFor);
  queue.sync(clips, keyFor);
  assert.equal(timers.size, 1, 'sync coalesces preparation');
  assert.equal(queue.status('clip-0'), 'queued');
  await fire();
  assert.equal(requests.length, 2);
  assert.equal(queue.status('clip-0'), 'loading');
  let complete = 0;
  while (cache.size < clips.length) {
    const pending = requests.slice(complete);
    assert(pending.length, 'all imported clips must eventually start automatically');
    complete = requests.length;
    for (const request of pending) request.resolve({ status: 'ready', version: 1 });
    await settle();
    if (cache.size < clips.length) await fire();
  }
  assert.equal(maximum(), 2, 'never exceed two evaluators');
  assert.equal(requests.length, clips.length, 'each clip loads exactly once');
  queue.sync(clips, keyFor);
  await fire();
  assert.equal(requests.length, clips.length, 'ready records are deduplicated');
  assert.equal(queue.status('clip-0'), '');
}

{
  const { queue, cache, requests } = harness({ concurrency: 1 });
  queue.sync([clip('same-id', 1)], keyFor);
  await fire();
  queue.sync([clip('same-id', 2)], keyFor);
  requests[0].resolve({ status: 'ready', source: 'old-source' });
  await settle();
  assert(!cache.has('same-id'), 'late old source response must be discarded');
  await fire();
  assert.equal(requests[1].item.version, 2);
  requests[1].resolve({ status: 'ready', source: 'current-source' });
  await settle();
  assert.equal(cache.get('same-id').source, 'current-source');

  queue.sync([clip('deleted-id')], keyFor);
  await fire();
  queue.sync([], keyFor);
  requests[2].resolve({ status: 'ready' });
  await settle();
  assert(!cache.has('deleted-id'), 'deleting a clip invalidates in-flight results');
  assert(!cache.has('same-id'), 'deleted ready cache is removed');
  assert.equal(queue.status('deleted-id'), '');
}

{
  const { queue, cache, requests } = harness();
  queue.sync([clip('first'), clip('second'), clip('third')], keyFor);
  await fire();
  queue.paused++;
  let drained = false;
  const draining = queue.drain().then(() => { drained = true; });
  requests[0].reject(Object.assign(new Error('cancelled'), { code: 'CANCELLED' }));
  await settle();
  assert(!drained, 'drain waits for every active evaluator');
  requests[1].reject(Object.assign(new Error('cancelled'), { code: 'CANCELLED' }));
  await draining;
  assert.equal(queue.running.size, 0);
  assert.equal(cache.size, 0, 'cancellation does not turn previews into permanent failures');
  await fire();
  assert.equal(requests.length, 2, 'paused queue stays idle after cancellation');
  queue.paused--;
  queue.schedule();
  await fire();
  assert.equal(requests.length, 4, 'resume retries cancelled previews');
  requests[2].resolve({ status: 'ready' });
  requests[3].resolve({ status: 'ready' });
  await settle();
  queue.suspended = true;
  await fire();
  assert.equal(requests.length, 4, 'user suspension prevents queued work');
  queue.retry('third');
  await fire();
  assert.equal(requests[4].item.id, 'third');
  requests[4].resolve({ status: 'ready' });
  await settle();
  assert.equal(cache.size, 3);
}

{
  const { queue, cache, requests } = harness({ concurrency: 1 });
  queue.sync([clip('transient')], keyFor);
  await fire();
  for (const code of ['BUSY', 'READ_IN_PROGRESS']) {
    requests.at(-1).reject(Object.assign(new Error('busy'), { code }));
    await settle();
    assert(!cache.has('transient'), `${code} is retryable`);
    assert.equal(queue.status('transient'), 'queued');
    await fire();
  }
  requests.at(-1).resolve({ status: 'blocked', reason: 'requires Avatar' });
  await settle();
  await fire();
  assert.equal(requests.length, 3, 'blocked evaluator result is not retried endlessly');
  queue.retry('transient');
  assert(!cache.has('transient'));
  await fire();
  requests.at(-1).reject(new Error('decoder failure'));
  await settle();
  assert.deepEqual(cache.get('transient'), { status: 'blocked', reason: 'decoder failure' });
  queue.retry('transient');
  await fire();
  requests.at(-1).resolve({ status: 'ready' });
  await settle();
  assert.equal(cache.get('transient').status, 'ready', 'explicit retry can recover a blocked clip');
}

{
  let allowed = false;
  const { queue, requests, cache } = harness({ canRun: () => allowed, concurrency: 1 });
  queue.sync([clip('a'), clip('b'), clip('c')], keyFor);
  await fire();
  assert.equal(requests.length, 0, 'engine busy state stops preparation');
  allowed = true;
  queue.retry('c');
  await fire();
  assert.equal(requests[0].item.id, 'c', 'hover/manual retry prioritizes a queued clip');
  queue.invalidate();
  requests[0].resolve({ status: 'ready' });
  await settle();
  assert.equal(cache.size, 0, 'invalidated settings discard all late results');
  assert.equal(queue.records.size, 0);
}

{
  // progress() drives the library's single progress bar.
  timers.clear();
  const cache = new Map();
  const queue = new PreviewQueue({ cache, load: async (item) => ({ status: 'ready', id: item.id }) });
  queue.sync([{ id: 'a' }, { id: 'b' }], (item) => item.id);
  assert.deepEqual(queue.progress(), { done: 0, total: 2 });
  await fire();
  assert.deepEqual(queue.progress(), { done: 2, total: 2 }, 'finished previews count as done');
}

timers.clear();
console.log('PreviewQueue concurrency, dedup, invalidation, cancellation, retry and progress checks passed');
