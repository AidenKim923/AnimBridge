// Bounded preparation of every imported clip. Identity changes discard late results.
export class PreviewQueue {
  constructor({ cache, load, changed = () => {}, canRun = () => true, concurrency = 2 }) {
    Object.assign(this, { cache, load, changed, canRun, concurrency });
    this.records = new Map();
    this.running = new Set();
    this.paused = 0;
    this.suspended = false;
    this.timer = null;
    this.priority = '';
  }
  sync(items, keyFor) {
    const ids = new Set(items.map((item) => item.id));
    for (const id of this.records.keys()) if (!ids.has(id)) {
      this.records.delete(id);
      this.cache.delete(id);
    }
    for (const item of items) {
      const key = keyFor(item), old = this.records.get(item.id);
      if (!old || old.key !== key || (old.done && !this.cache.has(item.id))) {
        this.cache.delete(item.id);
        this.records.set(item.id, { item, key, done: false, started: false });
      }
    }
    this.schedule();
  }
  invalidate() {
    this.records.clear();
    this.cache.clear();
  }
  retry(id) {
    const record = this.records.get(id);
    if (record && !record.started) {
      record.done = false;
      this.cache.delete(id);
    }
    this.suspended = false;
    this.priority = id;
    this.schedule();
  }
  status(id) {
    const record = this.records.get(id);
    return record && !record.done ? (record.started ? 'loading' : 'queued') : '';
  }
  schedule(delay = 0) {
    if (this.timer !== null) return;
    this.timer = setTimeout(() => { this.timer = null; this.pump(); }, delay);
  }
  pump() {
    if (this.paused || this.suspended || !this.canRun()) return;
    const queued = [...this.records.values()].filter((r) => !r.started && !r.done);
    queued.sort((a, b) => Number(b.item.id === this.priority) - Number(a.item.id === this.priority));
    while (this.running.size < this.concurrency && queued.length) {
      const record = queued.shift();
      record.started = true;
      const promise = Promise.resolve().then(() => this.load(record.item)).then((result) => {
        if (this.records.get(record.item.id) !== record) return;
        this.cache.set(record.item.id, result);
        record.done = true;
      }).catch((error) => {
        if (this.records.get(record.item.id) !== record) return;
        if (['BUSY', 'READ_IN_PROGRESS', 'CANCELLED'].includes(error.code)) return;
        this.cache.set(record.item.id, { status: 'blocked', reason: error.message });
        record.done = true;
      }).finally(() => {
        record.started = false;
        this.running.delete(promise);
        this.changed(record.item.id);
        this.schedule(100);
      });
      this.running.add(promise);
    }
  }
  // Overall background progress for the library: how many tracked clips have a finished preview.
  progress() {
    const records = [...this.records.values()];
    return { done: records.filter((r) => r.done).length, total: records.length };
  }
  async drain() { await Promise.allSettled([...this.running]); }
}
