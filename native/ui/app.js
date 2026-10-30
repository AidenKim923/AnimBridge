import { hostTransport } from './host-transport.js';
import { icon, folderArt } from './icons.js';
import { PoseViewer } from './viewer.js';
import { PreviewQueue } from './preview-queue.js';
import {
  t,
  setLocale,
  getLocale,
  normalizeLocale,
  formatNumber,
  formatDate,
  SUPPORTED_LOCALES,
} from './i18n.js';

const $ = (s) => document.querySelector(s),
  esc = (x) =>
    String(x ?? '').replace(
      /[&<>"']/g,
      (c) => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[c],
    );
const mappingKey = (x) =>
  JSON.stringify(Object.entries(x || {}).sort(([a], [b]) => a.localeCompare(b)));
const basename = (p) =>
  String(p || '')
    .replace(/\\/g, '/')
    .split('/')
    .pop();
const seconds = (n) =>
  Number.isFinite(+n) && +n > 0
    ? t('ui.001', { value0: formatNumber(+n, { maximumFractionDigits: 2 }) })
    : t('ui.002');
const clock = (n) => {
  const seconds = Math.max(0, +n || 0);
  return (
    formatNumber(Math.floor(seconds / 60), { minimumIntegerDigits: 2, useGrouping: false }) +
    ':' +
    formatNumber(seconds % 60, {
      minimumIntegerDigits: 2,
      minimumFractionDigits: 2,
      maximumFractionDigits: 2,
      useGrouping: false,
    })
  );
};
const date = formatDate;
function localizeDocument() {
  for (const element of document.querySelectorAll('[data-i18n]'))
    element.textContent = t(element.dataset.i18n);
}
function emphasizedText(key, parameter, text) {
  const marker = '\uFFF0';
  return t(key, { [parameter]: marker })
    .split(marker)
    .map(esc)
    .join(`<strong>${esc(text)}</strong>`);
}
function messageText(value, fallback = '') {
  if (typeof value === 'string') return value;
  const key = value?.messageKey || value?.errorKey;
  return key
    ? t(key, value.messageParams || value.errorParams || {})
    : value?.message || value?.error || fallback;
}
function sessionWarnings() {
  return (native.session.warnings || []).map((message) => {
    const metadata = native.session.warningMessages?.find((item) => item.message === message);
    return metadata ? messageText(metadata) : message;
  });
}
function pickerLabels(folder) {
  return {
    title: t(folder ? 'native.pickFolderTitle' : 'native.pickFileTitle'),
    filterLabel: t('native.supportedFilesFilter'),
    allFilesLabel: t('native.allFilesFilter'),
  };
}
const app = $('#app'),
  modal = $('#modal');
let serial = 0;
const requestSession = crypto.randomUUID();
const scrollPositions = new Map();
const pending = new Map(),
  previews = new Map(),
  metadata = new Map();
let previewRenderTimer = 0, cardClickTimer = 0, hoveredClip = '', hoverTime = 0, hoverTick = 0;
// Reads the user started explicitly (connect, validate, ...) own the top banner; background
// thumbnail/hover work is shown per card only, so it never shifts the layout or flickers.
let userReads = 0, queueActiveAt = 0, hoverPriorityTimer = 0, lastMarkup = '', pendingPatch = new Set();
function showReadBanner() {
  return !!native.readBusy && (userReads > 0 ||
    (previewQueue.running.size === 0 && !previewQueue.paused && Date.now() - queueActiveAt > 2000));
}
let viewerObserver = null;
let viewers = [],
  raf = 0,
  lastTick = 0,
  toastTimer;
let native = {
  session: { clips: [], warnings: [] },
  packages: [],
  selectedIds: [],
  settings: {
    language: 'en',
    theme: 'dark',
    uiScale: 100,
    python: 'python',
    outputRoot: '',
    skeleton: '',
    unityEditor: '',
    unreal_editor: '',
    unityProject: '',
  },
  jobs: [],
  history: [],
  busy: false,
  scan: { state: 'idle' },
  unity: { state: 'disconnected' },
  skeleton: { state: 'missing' },
};
let view = {
  page: 'home',
  active: '',
  // SOURCE LIBRARY scope: the root's Recent path, then a folder inside it.
  source: '',
  folder: '',
  // Tree rows the user expanded or collapsed, keyed by treeKey().
  tree: {},
  search: '',
  type: [],
  rig: [],
  ready: [],
  min: '',
  max: '',
  sort: 'name',
  layout: 'grid',
  filters: false,
  drawer: false,
  settingTab: 'general',
  jobId: '',
  error: '',
  previewBusy: false,
  connecting: false,
  validating: false,
  plan: null,
};
try {
  Object.assign(view, JSON.parse(localStorage.getItem('animbridge.view') || '{}'));
  view.page = 'home';
  view.error = '';
  view.plan = null;
  view.previewBusy = false;
  view.connecting = false;
  view.validating = false;
} catch {}
let selected = new Set(),
  draft = null,
  settingsDraft = null,
  settingsDirty = false,
  settingsSaving = false,
  play = { running: false, time: 0, speed: 1, loop: true, floor: true, bones: null },
  returnPage = 'library';
const previewQueue = new PreviewQueue({
  cache: previews,
  load: preparePreview,
  canRun: () => !native.busy && !view.validating && !view.connecting &&
    !['running', 'scanning', 'analyzing'].includes(native.scan?.state) &&
    (!native.readBusy || previewQueue.running.size > 0),
  changed: (id) => {
    const p = previews.get(id);
    if (p?.status === 'ready') {
      metadata.set(id, { ...(metadata.get(id) || {}), duration: p.duration, fps: p.source_fps || p.fps });
      if (id === view.active && view.page === 'preview' && !play.running) startPlay();
    }
    pendingPatch.add(id);
    if (!previewRenderTimer) previewRenderTimer = setTimeout(() => {
      previewRenderTimer = 0;
      const ids = [...pendingPatch];
      pendingPatch.clear();
      if (!patchThumbnails(ids)) render();
    }, 50);
  },
});
function previewOptions() {
  return {
    mapping: draft?.boneMap || {}, bone_map: draft?.boneMap || {}, include_mesh: true,
    max_frames: 90, max_mesh_vertices: 1000, max_point_frames: 60000,
    skeleton: draft?.skeleton || native.settings.skeleton,
    python: native.settings.python, blender: native.settings.blender,
    unityEditor: native.settings.unityEditor, unityProject: native.settings.unityProject,
    unreal_editor: native.settings.unreal_editor || '',
  };
}
// Show the skeleton overlay when the user chose so, or by default when there is no mesh to draw.
function showBones(p) {
  return play.bones ?? !p?.mesh;
}
function syncPreviews() {
  const options = JSON.stringify(previewOptions());
  previewQueue.concurrency = Math.max(1, Math.min(2, native.readConcurrency || 2));
  previewQueue.sync(clips().filter((c) => ['anim', 'fbx', 'baked'].includes(c.kind)),
    (c) => JSON.stringify([c.source, c.sourceHash, c.parserVersion, c.name, options]));
}
function invalidatePreviews() {
  previewQueue.invalidate();
  syncPreviews();
}
async function preparePreview(c) {
  queueActiveAt = Date.now();
  try {
    return await preparePreviewNow(c);
  } finally {
    queueActiveAt = Date.now();
  }
}
async function preparePreviewNow(c) {
  let p = await request('preview', { clipId: c.id, options: { ...previewOptions(), humanoid: typeOf(c) === 'humanoid' } });
  if (p.preview) p = p.preview;
  if (p.status === 'ok') p.status = 'ready';
  if (!p.status) p.status = p.frames?.length ? 'ready' : 'blocked';
  if (p.status === 'ready' && (!p.frames?.length || !p.bones?.length))
    throw new Error(t('ui.407'));
  return p;
}
let windowChrome = { maximized: false, active: true };
let appInfo = { version: '', links: { github: false, fairy: false, githubSponsors: false } };
let resizeGesture = null;
let resizeSerial = 0;
function applyWindowState(state) {
  windowChrome = { ...windowChrome, ...state };
  document.documentElement.classList.toggle('window-inactive', !windowChrome.active);
  document.documentElement.classList.toggle('window-maximized', windowChrome.maximized);
  document.documentElement.style.setProperty(
    '--resize-border',
    `${windowChrome.resizeBorderCss || 6}px`,
  );
  const button = $('#window-maximize');
  if (button) {
    const label = windowChrome.maximized ? t('ui.003') : t('ui.004');
    button.title = label;
    button.setAttribute('aria-label', label);
    button.innerHTML = windowGlyph(windowChrome.maximized ? 'restore' : 'maximize');
  }
}
function windowGlyph(command) {
  const path = {
    minimize: 'M2 7h10',
    maximize: 'M2.5 2.5h9v9h-9Z',
    restore: 'M4.5 2.5h7v7M2.5 4.5h7v7h-7Z',
    close: 'm3 3 8 8M11 3l-8 8',
  }[command];
  return `<svg viewBox="0 0 14 14" aria-hidden="true"><path d="${path}"/></svg>`;
}
function titlebar() {
  const customFrame = document.documentElement.dataset.customFrame !== 'false';
  const maximize = windowChrome.maximized ? t('ui.003') : t('ui.004');
  return `<header class="topbar">
    <span class="brand-mark"><img src="app-icon.png" alt="" aria-hidden="true" draggable="false"></span>
    <span>AnimBridge${appInfo.provenance?.testHooks ? ' · G2 Harness' : ''}</span>
    <span class="app-version" aria-label="${esc(t('ui.005'))}">${appInfo.version ? `v${esc(appInfo.version)}` : ''}</span>
    <div class="header-links" role="group" aria-label="${esc(t('ui.006'))}">
      <button class="header-link" data-external-link="github" title="GitHub${esc(appInfo.links.github ? t('ui.007') : t('ui.008'))}">${icon('github')}<span>GitHub</span></button>
      <button class="header-link" data-action="showWelcome" title="${esc(t('ui.498'))}">${icon('info')}<span>${esc(t('ui.498'))}</span></button>
      <button class="header-link" data-action="showSupport" title="${esc(t('ui.009'))}">${icon('heart', 'support-heart')}<span>${esc(t('ui.010'))}</span></button>
    </div>
    ${customFrame ? `<div class="window-controls" role="group" aria-label="${esc(t('ui.011'))}">
      <button id="window-minimize" class="window-control" data-window-command="minimize" title="${esc(t('ui.012'))}" aria-label="${esc(t('ui.012'))}">${windowGlyph('minimize')}</button>
      <button id="window-maximize" class="window-control" data-window-command="maximize" title="${maximize}" aria-label="${maximize}">${windowGlyph(windowChrome.maximized ? 'restore' : 'maximize')}</button>
      <button id="window-close" class="window-control close-window" data-window-command="close" title="${esc(t('ui.013'))}" aria-label="${esc(t('ui.013'))}">${windowGlyph('close')}</button>
    </div>` : ''}
  </header>`;
}

async function openExternalLink(link) {
  if (!appInfo.links[link]) {
    externalLinkFeedback(link === 'github' ? t('ui.014') : t('ui.015'));
    return;
  }
  try {
    await request('openExternalLink', { link });
  } catch (error) {
    externalLinkFeedback(error.message);
  }
}

function externalLinkFeedback(message) {
  const status = $('#support-status');
  if (modal.open && status) status.textContent = message;
  else toast(message);
}

function showSupport() {
  const caller = document.activeElement;
  if (modal.open) modal.close();
  modal.classList.add('support-dialog');
  modal.setAttribute('aria-describedby', 'support-description');
  modal.innerHTML = `<button class="icon-button support-close" data-close-support aria-label="${esc(t('ui.013'))}">${icon('close')}</button>
    <h2 id="modal-title">${esc(t('ui.016'))}</h2>
    <p id="support-description">${emphasizedText('ui.017', 'free', t('ui.018'))}<br>${esc(t('ui.020'))}<br>${esc(t('ui.021'))}</p>
    <div class="support-methods">
      <button class="support-method" data-support-link="fairy" title="Fairy${esc(appInfo.links.fairy ? t('ui.007') : t('ui.008'))}">
        <span class="support-method-icon fairy">${icon('heart')}</span>
        <span class="support-method-copy"><strong>Fairy</strong><span>${esc(t('ui.022'))}</span></span>
        ${icon('chevron')}
      </button>
      <button class="support-method" data-support-link="githubSponsors" title="GitHub Sponsors${esc(appInfo.links.githubSponsors ? t('ui.007') : t('ui.008'))}">
        <span class="support-method-icon github-sponsors">${icon('heart')}</span>
        <span class="support-method-copy"><strong>GitHub Sponsors</strong><span>${esc(t('ui.023'))}</span></span>
        ${icon('chevron')}
      </button>
    </div>
    <p id="support-status" class="support-status" role="status" aria-live="polite"></p>`;
  const finish = () => {
    modal.close();
    modal.classList.remove('support-dialog');
    modal.removeAttribute('aria-describedby');
    modal.onclick = null;
    modal.oncancel = null;
    if (caller?.isConnected) caller.focus();
    else app.querySelector('[data-action="showSupport"]')?.focus();
  };
  modal.onclick = (event) => {
    if (event.target.closest('[data-close-support]')) finish();
    const link = event.target.closest('[data-support-link]');
    if (link) openExternalLink(link.dataset.supportLink);
  };
  modal.oncancel = (event) => {
    event.preventDefault();
    finish();
  };
  modal.showModal();
  modal.querySelector('[data-close-support]').focus();
}

// First-run guide: four short steps and a support button. Shown once
// (remembered in localStorage) and reopened from the "How to use" header link.
const WelcomeSeenKey = 'animbridge.welcomeSeen';
const welcomeSteps = [
  ['folderOpen', 'ui.489', 'ui.490'],
  ['eye', 'ui.491', 'ui.492'],
  ['monitor', 'ui.493', 'ui.494'],
  ['upload', 'ui.495', 'ui.496'],
];

function welcomeSeen() {
  try {
    return localStorage.getItem(WelcomeSeenKey) === '1';
  } catch {
    return false; // storage unavailable: showing the guide again is harmless
  }
}

function showWelcome() {
  return new Promise((resolve) => {
    const caller = document.activeElement;
    if (modal.open) modal.close();
    modal.classList.add('welcome-dialog');
    const steps = welcomeSteps
      .map(([iconName, title, text], index) => [
        `<li class="welcome-step">`,
        `<span class="welcome-step-icon">${icon(iconName)}</span>`,
        `<div><strong>${index + 1}. ${esc(t(title))}</strong><p>${esc(t(text))}</p></div>`,
        `</li>`,
      ].join(''))
      .join('');
    modal.innerHTML = [
      `<h2 id="modal-title">${esc(t('ui.487'))}</h2>`,
      `<p class="welcome-intro">${esc(t('ui.488'))}</p>`,
      `<ol class="welcome-steps">${steps}</ol>`,
      `<div class="welcome-support">`,
      `<span>${esc(t('ui.497'))}</span>`,
      `<button class="btn" data-welcome="support">${icon('heart', 'support-heart')}${esc(t('ui.010'))}</button>`,
      `</div>`,
      `<div class="actions"><button class="btn primary" data-welcome="start">${esc(t('ui.499'))}</button></div>`,
    ].join('');
    const finish = (next) => {
      try { localStorage.setItem(WelcomeSeenKey, '1'); } catch { /* storage unavailable */ }
      modal.close();
      modal.classList.remove('welcome-dialog');
      modal.onclick = null;
      modal.oncancel = null;
      if (next === 'support') showSupport();
      else if (caller?.isConnected) caller.focus();
      resolve();
    };
    modal.onclick = (event) => {
      const choice = event.target.closest('[data-welcome]')?.dataset.welcome;
      if (choice) finish(choice);
    };
    modal.oncancel = (event) => {
      event.preventDefault();
      finish('start');
    };
    modal.showModal();
    modal.querySelector('[data-welcome="start"]').focus();
  });
}

const exclusiveActions = new Set([
  'chooseFiles', 'chooseFolder', 'saveReport', 'scan', 'drop', 'plan', 'start', 'retryFailed', 'saveSettings', 'clearCache',
  'validateSkeleton', 'connectUnity', 'convertUnrealAsset',
]);
async function request(action, payload = {}, files = null) {
  if (!exclusiveActions.has(action)) return sendRequest(action, payload, files);
  previewQueue.paused++;
  try {
    // A WebView reload can leave native reads whose promises belonged to the
    // previous document. Drain the host as well as this document's queue.
    let state = await sendRequest('getState');
    if (previewQueue.running.size || state.readBusy) {
      await sendRequest('cancel');
      await previewQueue.drain();
      const deadline = Date.now() + 90000;
      do {
        state = await sendRequest('getState');
        if (!state.readBusy) break;
        if (Date.now() > deadline) throw new Error(t('ui.025'));
        await new Promise((resolve) => setTimeout(resolve, 40));
      } while (true);
    }
    userReads++;
    try {
      return await sendRequest(action, payload, files);
    } finally {
      userReads--;
      render();
    }
  } finally {
    previewQueue.paused--;
    previewQueue.schedule(150);
  }
}
function sendRequest(action, payload = {}, files = null) {
  return new Promise((resolve, reject) => {
    const id = `${requestSession}:${++serial}`;
    const timeout = setTimeout(
      () => {
        pending.delete(id);
        reject(new Error(t('ui.025')));
      },
      action === 'preview' || action === 'connectUnity' || action === 'validateSkeleton'
        ? 660000
        : action === 'convertUnrealAsset'
          ? 900000 // A headless Unreal Editor may need minutes to load a large project.
          : 90000,
    );
    pending.set(id, { resolve, reject, timeout });
    hostTransport.send({ id, action, payload }, files).catch((error) => {
      clearTimeout(timeout);
      pending.delete(id);
      reject(error);
    });
  });
}
function accept(data) {
  if (typeof data === 'string') {
    try {
      data = JSON.parse(data);
    } catch {
      return;
    }
  }
  if (data.id !== undefined && pending.has(String(data.id))) {
    const p = pending.get(String(data.id));
    clearTimeout(p.timeout);
    pending.delete(String(data.id));
    if (data.ok) p.resolve(data.result);
    else
      p.reject(
        Object.assign(
          new Error(
            typeof data.error === 'string' ? data.error : messageText(data.error, t('ui.026')),
          ),
          { code: data.error?.code },
        ),
      );
    return;
  }
  if (data.event === 'nativeDrop') {
    dragDepth = 0;
    $('#drop-overlay')?.classList.remove('visible');
    recordPageChange('analysis');
    view.page = 'analysis';
    native.scan = { state: 'scanning', path: '' };
    render();
  } else if (data.event === 'windowState') {
    applyWindowState(data.data);
  } else if (data.event === 'state') {
    applyState(data.data);
  } else if (data.event === 'progress') {
    if (data.data?.state) applyState(data.data.state);
    else if (data.data?.job) {
      upsertJob(data.data.job);
      render();
    } else
      request('getState')
        .then(applyState)
        .catch(() => {});
  } else if (data.event === 'result') {
    request('getState')
      .then((s) => {
        applyState(s);
        if (view.page === 'queue' && !native.busy) go('complete');
      })
      .catch(showError);
  } else if (data.event === 'updateProgress') {
    showUpdateProgress(data.data || {});
  } else if (data.event === 'error') {
    showError(messageText(data.data));
  } else if (data.ok === false) {
    showError(messageText(data.error, t('ui.027')));
  }
}
hostTransport.subscribe(accept);
function upsertJob(job) {
  let i = native.jobs.findIndex((j) => jobId(j) === jobId(job));
  if (i < 0) native.jobs.push(job);
  else native.jobs[i] = job;
}
function applyState(s) {
  if (!s) return;
  if (s.session?.clips) {
    for (const c of s.session.clips) {
      const old = native.session.clips.find((x) => x.id === c.id);
      if (old && (old.sourceHash !== c.sourceHash || old.parserVersion !== c.parserVersion)) {
        previews.delete(c.id);
        metadata.delete(c.id);
      }
    }
  }
  if (s.settings?.skeleton !== undefined && s.settings.skeleton !== native.settings.skeleton)
    previewQueue.invalidate();
  const wasScanning = ['running', 'scanning', 'analyzing'].includes(native.scan?.state);
  native = {
    ...native,
    ...s,
    session: { ...native.session, ...s.session },
    settings: { ...native.settings, ...s.settings },
  };
  native.settings.language = normalizeLocale(native.settings.language);
  setLocale(native.settings.language);
  localizeDocument();
  if (s.selectedIds) selected = new Set(s.selectedIds);
  else if (s.session?.clips)
    selected = new Set(s.session.clips.filter((c) => c.selected).map((c) => c.id));
  if (!draft) draft = defaultExport();
  if (!settingsDirty) settingsDraft = { ...native.settings };
  if (
    view.page === 'queue' &&
    !native.busy &&
    ['completed', 'completed-with-skips', 'cancelled', 'failed', 'interrupted'].includes(
      currentJob()?.state,
    )
  )
    view.page = 'complete';
  if (
    wasScanning &&
    !['running', 'scanning', 'analyzing'].includes(native.scan?.state) &&
    view.page === 'analysis'
  )
    toast(t('ui.028'));
  if (!native.busy && !['running', 'scanning', 'analyzing'].includes(native.scan?.state)) {
    if (wasScanning) previewQueue.suspended = false;
    syncPreviews();
  }
  render();
}
function defaultExport() {
  return {
    mode: 'extractOriginal',
    outputRoot: native.settings.outputRoot || '',
    targetProfile: 'unreal',
    direction: 'unity-to-unreal',
    collisionPolicy: 'rename',
    explicitOverwrite: false,
    skeleton: native.settings.skeleton || '',
    sampleRate: 0,
    rootMotion: 'preserve',
    boneMap: { ...(native.settings.boneMap || {}) },
    python: native.settings.python || 'python',
    blender: native.settings.blender || '',
    targetEditor: '',
    targetProject: '',
    targetSkeleton: '',
    targetFolder: '',
  };
}
function saveView() {
  try {
    localStorage.setItem(
      'animbridge.view',
      JSON.stringify({
        search: view.search,
        source: view.source,
        folder: view.folder,
        tree: view.tree,
        sort: view.sort,
        layout: view.layout,
        type: view.type,
        rig: view.rig,
        ready: view.ready,
        min: view.min,
        max: view.max,
        active: view.active,
      }),
    );
  } catch {}
}
function toast(text) {
  $('#toast').textContent = text;
  $('#toast').classList.add('visible');
  clearTimeout(toastTimer);
  toastTimer = setTimeout(() => $('#toast').classList.remove('visible'), 4200);
}
function showError(e) {
  view.error = typeof e === 'object' ? messageText(e, String(e)) : String(e);
  render();
  toast(view.error);
}
// In-app page history, so the mouse back button, Alt+Left and Backspace work like a browser's.
// `back` holds the pages we came from (newest last); `forward` the ones we stepped back from.
const pageHistory = { back: [], forward: [] };
const PAGE_HISTORY_LIMIT = 40;

// Call right before view.page changes. `via` is 'back' or 'forward' when replaying history,
// and empty for a normal navigation (which also discards the forward entries, like a browser).
function recordPageChange(next, via = '') {
  const from = view.page;
  if (next === from) return;
  if (via === 'back' && pageHistory.back.at(-1) === next) {
    pageHistory.back.pop();
    pageHistory.forward.push(from);
    return;
  }
  if (via === 'forward' && pageHistory.forward.at(-1) === next) {
    pageHistory.forward.pop();
  } else {
    pageHistory.forward.length = 0;
  }
  // Never stack the same page twice in a row.
  if (pageHistory.back.at(-1) !== from) pageHistory.back.push(from);
  if (pageHistory.back.length > PAGE_HISTORY_LIMIT) pageHistory.back.shift();
}
// A finished export run is no longer a page worth returning to.
function isStaleHistoryPage(page) {
  return page === 'queue' && !native.busy;
}
// Closes the open dialog or detail drawer first; otherwise steps through the page history.
// Returns true when something happened, so callers know whether to swallow the key or button.
function navigateHistory(direction) {
  if (modal.open) {
    // Reuse the dialog's own cancel handler so it cleans up exactly like Escape does.
    const cancel = new Event('cancel', { cancelable: true });
    modal.dispatchEvent(cancel);
    if (!cancel.defaultPrevented) modal.close();
    return true;
  }
  if (view.drawer) {
    view.drawer = false;
    render();
    return true;
  }
  const stack = pageHistory[direction];
  while (stack.length && (stack.at(-1) === view.page || isStaleHistoryPage(stack.at(-1)))) stack.pop();
  if (!stack.length) return false;
  go(stack.at(-1), { via: direction });
  return true;
}
// Explicit "return to X" buttons: when X is where we just came from, step back instead of
// pushing a loop (library -> avatar -> library -> avatar ...).
function goOrBack(page) {
  if (pageHistory.back.at(-1) === page) go(page, { via: 'back' });
  else go(page);
}
function go(page, { via = '' } = {}) {
  if (settingsDirty && view.page === 'settings' && page !== 'settings') {
    confirmDialog(t('ui.029'), t('ui.030'), [
      { label: t('ui.031'), value: 'stay' },
      { label: t('ui.032'), value: 'discard' },
      { label: t('ui.033'), value: 'save', primary: true },
    ]).then(async (v) => {
      if (v === 'save') {
        if (await saveSettings()) go(page, { via });
      } else if (v === 'discard') {
        settingsDirty = false;
        settingsDraft = { ...native.settings };
        go(page, { via });
      }
    });
    return;
  }
  recordPageChange(page, via);
  scrollPositions.set(view.page, $('.page')?.scrollTop || 0);
  if (page !== 'preview' || view.page !== page) stopPlay();
  hoverPreview('');
  view.page = page;
  view.error = '';
  view.drawer = false;
  if (page === 'preview') startPlay();
  if (page === 'settings' && !settingsDirty) settingsDraft = { ...native.settings };
  if (page === 'export' && !draft) draft = defaultExport();
  saveView();
  render();
  if ($('.page')) $('.page').scrollTop = scrollPositions.get(page) || 0;
  $('#main-title')?.focus({ preventScroll: true });
}
function clipData(c) {
  const m = metadata.get(c.id) || {};
  return {
    ...c,
    ...m,
    name: c.name,
    id: c.id,
    source: c.source,
    virtual_path: c.virtual_path,
    kind: c.kind,
    capabilities: c.capabilities,
    route: m.route || c.route,
  };
}
function clips() {
  return (native.session.clips || []).map(clipData);
}
function active() {
  return clips().find((c) => c.id === view.active);
}
function typeOf(c) {
  if (
    c.rigType === 'humanoid' ||
    c.type === 'humanoid' ||
    c.is_humanoid ||
    c.humanoid ||
    c.route === 'requires-unity-bake'
  )
    return 'humanoid';
  if (c.kind === 'fbx') return 'fbx';
  if (c.kind === 'baked') return 'baked';
  return c.kind === 'anim' ? 'generic' : c.kind || 'unknown';
}
function typeLabel(c) {
  return (
    {
      humanoid: t('ui.034'),
      fbx: 'FBX',
      generic: t('ui.035'),
      baked: t('ui.036'),
      uasset: 'UASSET',
    }[typeOf(c)] || t('ui.002')
  );
}
function ready(c) {
  if (c.capabilities?.bake === false || c.kind === 'uasset' || c.route === 'requires-engine-export')
    return 'blocked';
  if (c.capabilities?.bake === true) return 'ready';
  return 'connection';
}
function readyLabel(c) {
  return { blocked: t('ui.037'), connection: t('ui.038'), ready: t('ui.039') }[ready(c)];
}
function canExtract(c) {
  return c.capabilities?.extract === true;
}
function selectedClips() {
  return clips().filter((c) => selected.has(c.id));
}
function visibleClips() {
  const inScope = scopeFilter();
  let a = clips().filter(
    (c) =>
      inScope(c) &&
      (!view.search || c.name.toLocaleLowerCase(getLocale()).includes(view.search.toLocaleLowerCase(getLocale()))) &&
      (!view.type.length || view.type.includes(typeOf(c))) &&
      (!view.rig.length || view.rig.includes(c.rigType || 'unknown')) &&
      (!view.ready.length || view.ready.includes(ready(c))) &&
      ((view.min === '' && view.max === '') ||
        (c.duration > 0 &&
          (view.min === '' || c.duration >= +view.min) &&
          (view.max === '' || c.duration <= +view.max))),
  );
  a.sort((a, b) =>
    view.sort === 'duration'
      ? (a.duration || Infinity) - (b.duration || Infinity)
      : view.sort === 'type'
        ? typeLabel(a).localeCompare(typeLabel(b))
        : a.name.localeCompare(b.name, getLocale()),
  );
  return a;
}
function jobId(j) {
  return j?.attemptId || j?.attempt_id || j?.jobId || j?.job_id || j?.id || '';
}
function jobResults(j) {
  if (!j) return [];
  const rows = j.results || j.items || [],
    planned = j.plan?.jobs || [];
  if (!planned.length) return rows;
  return planned.map(
    (p) =>
      rows.find((r) => r.id === p.clip?.id) ||
      (j.current?.id === p.clip?.id && !['failed', 'interrupted'].includes(j.state)
        ? j.current
        : {
            id: p.clip?.id,
            name: p.clip?.name,
            status: ['failed', 'interrupted'].includes(j.state)
              ? 'error'
              : j.state === 'cancelled'
                ? 'cancelled'
                : j.state === 'completed-with-skips'
                  ? 'skipped'
                  : j.state === 'completed'
                    ? 'error'
                    : 'queued',
            message:
              messageText(j) ||
              (['failed', 'interrupted'].includes(j.state)
                ? t('ui.040')
                : j.state === 'cancelled'
                  ? t('ui.041')
                  : j.state === 'completed-with-skips'
                    ? t('ui.042')
                    : j.state === 'completed'
                      ? t('ui.043')
                      : t('ui.044')),
          }),
  );
}
function currentJob() {
  const all = [...(native.jobs || []), ...(native.history || [])];
  return all.find((j) => jobId(j) === view.jobId) || native.jobs.at(-1) || native.history[0];
}
function resultTerminal(r) {
  return [
    'ok',
    'warning',
    'imported',
    'completed',
    'ready-for-unity-import',
    'error',
    'blocked',
    'skipped',
    'cancelled',
    'canceled',
  ].includes(r.status);
}
function good(r) {
  return ['ok', 'warning', 'imported', 'completed', 'ready-for-unity-import'].includes(r.status);
}
function statusText(s) {
  return (
    {
      ok: t('ui.045'),
      warning: t('ui.046'),
      imported: t('ui.047'),
      completed: t('ui.045'),
      failed: t('ui.048'),
      interrupted: t('ui.049'),
      error: t('ui.048'),
      blocked: t('ui.050'),
      skipped: t('ui.051'),
      cancelled: t('ui.052'),
      canceled: t('ui.052'),
      queued: t('ui.053'),
      running: t('ui.054'),
      validating: t('ui.055'),
      baking: t('ui.056'),
      writing: t('ui.057'),
      verifying: t('ui.058'),
      'ready-for-unity-import': t('ui.059'),
    }[s] ||
    esc(s) ||
    t('ui.053')
  );
}
function badge(text, ico = '', cls = '') {
  return `<span class="badge ${cls}">${ico ? icon(ico) : ''}${esc(text)}</span>`;
}
function button(label, action, ico = '', cls = '', extra = '') {
  return `<button class="btn ${cls}" data-action="${action}" ${extra}>${ico ? icon(ico) : ''}${esc(label)}</button>`;
}
function notice(title, text, ico = 'info') {
  return `<div class="notice">${icon(ico)}<div><strong>${esc(title)}</strong><p>${esc(text)}</p></div></div>`;
}
function empty(title, text, action = '', label = t('ui.060'), ico = 'folder') {
  return [
    `<div class="empty-state">${icon(ico)}<h2>${esc(title)}`,
    `</h2><p>${esc(text)}</p>${action ? button(label, action, 'folderOpen', 'primary') : ''}`,
    `</div>`,
  ].join('');
}
function heading(title, subtitle = '', eyebrow = '') {
  return [
    `<div class="page-heading"><div>${eyebrow ? `<div class="eyebrow">${eyebrow}</div>` : ''}`,
    `<h1 id="main-title" tabindex="-1">${esc(title)}</h1>`,
    `${subtitle ? `<p>${esc(subtitle)}</p>` : ''}</div>`,
    `${view.page === 'library' ? button(t('ui.061'), 'toggleDetail', 'info', 'ghost drawer-toggle') : ''}`,
    `</div>`,
  ].join('');
}
function labelInput(label, id, value = '', hint = '', placeholder = '') {
  return [
    `<div class="field"><label for="${id}">${esc(label)}`,
    `</label><input id="${id}" type="text" value="${esc(value)}" readonly placeholder="${esc(placeholder)}" ${hint ? `aria-describedby="${id}-hint"` : ''}>`,
    `${hint ? `<span class="hint" id="${id}-hint">${esc(hint)}</span>` : ''}`,
    `</div>`,
  ].join('');
}
// The "detected automatically" note applies only while the field still shows the editor that
// auto-discovery connected; a user-chosen editor shows no note.
function autoDetectedHint(auto, currentPath, key) {
  return auto?.state === 'connected' && auto.path && auto.path === currentPath ? t(key) : '';
}
function pickerField(label, id, value = '', action = 'pickFolder', hint = '') {
  if (native.runtimeReadOnly && ['setting-python', 'setting-blender'].includes(id)) return `<div class="field"><label for="${id}">${esc(label)}</label><input id="${id}" value="${esc(value)}" readonly></div>`;
  return [
    `<div class="field"><label for="${id}">${esc(label)}`,
    `</label><div class="input-row"><input id="${id}" type="text" value="${esc(value)}" readonly>`,
    `<button class="btn" data-action="${action}" data-field="${id}" aria-label="${esc(t('ui.457', { label }))}">`,
    `${icon('folderOpen')}${esc(t('ui.063'))}</button>`,
    `</div>${hint ? `<span class="hint">${esc(hint)}</span>` : ''}`,
    `</div>`,
  ].join('');
}
function rail() {
  return [
    `<nav class="rail" aria-label="${esc(t('ui.064'))}">`,
    `${[
      ['home', 'home', t('ui.065')],
      ['library', 'library', t('ui.066')],
      ['queue', 'transfer', t('ui.067')],
      ['history', 'chart', t('ui.068')],
    ]
      .map(
        ([p, i, t]) =>
          [
            `<button data-page="${p}" class="${view.page === p || (p === 'library' && ['analysis', 'preview', 'avatar', 'unity', 'export'].includes(view.page)) ? 'active' : ''}" title="${esc(t)}" aria-label="${esc(t)}" ${view.page === p ? 'aria-current="page"' : ''}>`,
            `${icon(i)}${p === 'queue' && native.busy ? '<span class="job-dot"></span>' : ''}`,
            `</button>`,
          ].join(''),
      )
      .join(
        '',
      )}`,
    `<button class="bottom ${view.page === 'settings' ? 'active' : ''}" data-page="settings" aria-label="${esc(t('ui.069'))}" title="${esc(t('ui.069'))}">`,
    `${icon('settings')}</button></nav>`,
  ].join('');
}
// SOURCE LIBRARY model. Every dropped package, folder or file is one root
// (its Recent row). A clip belongs to a root through its packageId, so two
// sources that both contain "Assets/Animations" never mix.
function samePath(a, b) {
  const norm = (p) => {
    const text = String(p || '').replace(/\\/g, '/').replace(/\/+$/, '');
    // Windows paths are case-insensitive.
    return document.documentElement.dataset.platform === 'windows' ? text.toLowerCase() : text;
  };
  return !!a && norm(a) === norm(b);
}
function sourceRoots() {
  const loaded = native.session.packages || [];
  const all = clips();
  return (native.packages || []).map((row) => {
    const path = row.path || row.sourcePath;
    const ids = new Set(loaded.filter((p) => samePath(p.sourcePath, path)).map((p) => p.packageId));
    return {
      path,
      name: row.name || basename(path),
      directory: !!row.directory,
      clips: all.filter((c) => ids.has(c.packageId)),
    };
  });
}
function currentRoot() {
  return view.source ? sourceRoots().find((r) => samePath(r.path, view.source)) : null;
}
// Predicate for the library: clips of the open root, below the open folder.
function scopeFilter() {
  const root = currentRoot();
  if (!root) return () => true;
  const ids = new Set(root.clips.map((c) => c.id));
  const prefix = view.folder ? view.folder.replace(/\/$/, '') + '/' : '';
  return (c) => ids.has(c.id) && (!prefix || String(c.virtual_path || '').startsWith(prefix));
}
// Every folder (and its ancestors) that holds at least one clip of a root.
function rootFolders(root) {
  const folders = new Set();
  for (const c of root.clips) {
    const parts = String(c.virtual_path || '').split('/').slice(0, -1);
    for (let i = 1; i <= parts.length; i++) folders.add(parts.slice(0, i).join('/'));
  }
  return [...folders].sort((a, b) => a.localeCompare(b, getLocale()));
}
function treeKey(root, folder) {
  return root.path + '\n' + folder;
}
// Roots start expanded and folders collapsed until the user toggles them;
// the folder that is open in the library always shows its parents.
function treeOpen(root, folder) {
  const key = treeKey(root, folder);
  if (key in view.tree) return view.tree[key];
  if (samePath(view.source, root.path) && view.folder.startsWith(folder ? folder + '/' : '')) return true;
  return !folder;
}
function treeRow(root, folder, depth, hasChildren, count) {
  const isRoot = !folder;
  const open = hasChildren && treeOpen(root, folder);
  const active = samePath(view.source, root.path) && view.folder === folder;
  const label = isRoot ? root.name : basename(folder);
  const fileRoot = isRoot && !root.directory && !/\.unitypackage$/i.test(root.name);
  return [
    `<div class="tree-row" role="treeitem" aria-level="${depth + 1}"`,
    hasChildren ? ` aria-expanded="${open}"` : '',
    ` style="padding-inline-start:${Math.min(depth, 6) * 12}px">`,
    hasChildren
      ? [
          `<button class="tree-toggle" data-action="toggleTree" data-key="${esc(treeKey(root, folder))}" data-open="${open}"`,
          ` aria-label="${esc(t(open ? 'ui.548' : 'ui.549', { name: label }))}">`,
          `${icon(open ? 'down' : 'chevron')}</button>`,
        ].join('')
      : `<span class="tree-toggle-space"></span>`,
    `<button class="tree-item ${active ? 'active' : ''}" data-action="openSource"`,
    ` data-source="${esc(root.path)}" data-value="${esc(folder)}"`,
    isRoot ? ` data-source-root="${esc(root.path)}" aria-haspopup="menu"` : '',
    ` title="${esc(isRoot ? root.path : folder)}">`,
    `${icon(fileRoot ? 'file' : 'folder')}<span class="label">${esc(label)}</span>`,
    `<span class="tree-count">${formatNumber(count)}</span></button></div>`,
  ].join('');
}
function sourceTree(root) {
  const folders = rootFolders(root);
  const children = (parent) => folders.filter((f) => f.split('/').slice(0, -1).join('/') === parent);
  const countUnder = (folder) =>
    root.clips.filter((c) => String(c.virtual_path || '').startsWith(folder + '/')).length;
  const branch = (folder, depth) => {
    const kids = children(folder);
    const count = folder ? countUnder(folder) : root.clips.length;
    const rows = [treeRow(root, folder, depth, kids.length > 0, count)];
    if (kids.length && treeOpen(root, folder)) for (const kid of kids) rows.push(branch(kid, depth + 1));
    return rows.join('');
  };
  return branch('', 0);
}
function sidebar() {
  let content = '';
  if (view.page === 'home') {
    content = [
      `<div class="sidebar-header"><h2>${esc(t('ui.070'))}`,
      `</h2></div><div class="sidebar-nav"><button class="side-item active" data-page="home">`,
      `${icon('home')}${esc(t('ui.071'))}${icon('chevron', 'chevron')}`,
      `</button><button class="side-item" data-page="library">`,
      `${icon('library')}${esc(t('ui.072'))}</button><button class="side-item" data-page="history">`,
      `${icon('clock')}${esc(t('ui.068'))}</button></div>`,
      `<div class="sidebar-section">${esc(t('ui.073'))}`,
      `</div><div class="sidebar-nav"><button class="side-item" data-page="unity">`,
      `${icon('link')}${esc(t('ui.074'))}</button></div>`,
    ].join('');
  } else if (view.page === 'settings') {
    content = [
      `<div class="sidebar-header"><h2>${esc(t('ui.069'))}`,
      `</h2></div><div class="sidebar-nav">`,
      `${[
        ['general', 'settings', t('ui.075')],
        ['export', 'upload', t('ui.076')],
        ['unity', 'link', t('ui.074')],
        ['storage', 'database', t('ui.077')],
      ]
        .map(
          ([p, i, t]) =>
            `<button class="side-item ${view.settingTab === p ? 'active' : ''}" data-action="settingTab" data-value="${p}">${icon(i)}${esc(t)}</button>`,
        )
        .join('')}`,
      `</div>`,
    ].join('');
  } else {
    content = [
      `<div class="sidebar-header"><h2>${esc(t('ui.078'))}`,
      `</h2><button class="icon-button" data-action="openFiles" aria-label="${esc(t('ui.079'))}">`,
      `${icon('plus')}</button></div><div class="sidebar-nav">`,
      `<button class="side-item ${!view.source ? 'active' : ''}" data-action="openSource" data-source="" data-value="">`,
      `${icon('library')}${esc(t('ui.080'))}<span class="tree-count">`,
      `${formatNumber(clips().length)}</span></button></div>`,
      `<div class="sidebar-section">${esc(t('ui.081'))}`,
      `</div><div class="tree" role="tree" aria-label="${esc(t('ui.081'))}">`,
      `${sourceRoots().map(sourceTree).join('')}`,
      `${!clips().length ? `<p class="small muted" style="padding:18px 10px">${esc(t('ui.082'))}</p>` : ''}`,
      `</div>`,
    ].join('');
  }
  return [
    `<aside class="sidebar" aria-label="${esc(t('ui.083'))}">`,
    `${content}<div class="spacer"></div></aside>`,
  ].join('');
}
function scopeLabel() {
  const root = currentRoot();
  if (!root) return '';
  return view.folder ? `${root.name} / ${view.folder}` : root.name;
}
function breadcrumb() {
  const names = {
    home: t('ui.070'),
    analysis: t('ui.085'),
    library: t('ui.066'),
    preview: t('ui.086'),
    avatar: t('ui.087'),
    unity: t('ui.074'),
    export: t('ui.088'),
    queue: t('ui.067'),
    complete: t('ui.089'),
    errors: t('ui.090'),
    settings: t('ui.069'),
    history: t('ui.068'),
  };
  return [
    `<div class="workspace-top"><div class="breadcrumb">`,
    `<button data-page="home">${esc(t('ui.091'))}</button>`,
    `${icon('chevron')}<span>${esc(scopeLabel() || names[view.page])}`,
    `</span>${view.page === 'preview' && active() ? icon('chevron') + `<span>${esc(active().name)}</span>` : ''}`,
    `</div>${button(t('ui.060'), 'openFiles', 'folderOpen', 'primary')}`,
    `</div>`,
  ].join('');
}
function home() {
  const recent = native.packages || [];
  return [
    `<div class="home-hero">${heading(t('ui.092'), t('ui.093'), t('ui.094'))}`,
    `<div class="dropzone" id="home-drop"><div class="dropzone-inner">`,
    `${folderArt()}<h2>${esc(t('ui.095'))}</h2><p>${esc(t('ui.096'))}`,
    `</p>${button(t('ui.060'), 'openFiles', 'folderOpen', 'primary')}`,
    `<p>${icon('lock')}${esc(t('ui.097'))}</p></div></div>`,
    `<div class="section-title"><h2>${esc(t('ui.098'))}`,
    `</h2><button class="link-button" data-page="library">`,
    `${esc(t('ui.099'))}${icon('chevron')}</button></div>`,
    `${
      recent.length
        ? [
          `<div class="recent-grid">`,
          `${recent
              .slice(-6)
              .reverse()
              .map(
                (p) =>
                  [
                    `<button class="recent-card" data-action="openRecent" data-value="${esc(p.path || p.sourcePath)}" title="${esc(p.path || p.sourcePath)}">`,
                    `${folderArt('person')}<span class="name">${esc(p.name || basename(p.path || p.sourcePath))}`,
                    `</span><small>${esc(p.sourceState === 'permissionDenied' ? t('ui.463') : p.sourceState === 'unavailable' ? t('ui.464') : p.approvalState === 'stale' ? t('ui.465') : p.exists === false ? t('ui.100') : date(p.scannedAt) || t('ui.101'))}`,
                    `</small></button>`,
                  ].join(''),
              )
              .join('')}`,
          `</div>`,
        ].join('')
        : `<div class="empty-recent">${icon('clock')}<div><h3>${esc(t('ui.102'))}</h3><p>${esc(t('ui.103'))}</p></div></div>`
    }`,
    `<div class="workflow-strip"><div><div class="step-label">`,
    `${esc(t('ui.104'))}</div><h3>${esc(t('ui.105'))}`,
    `</h3><p>${esc(t('ui.106'))}</p></div><div><div class="step-label">`,
    `${esc(t('ui.107'))}</div><h3>${esc(t('ui.108'))}`,
    `</h3><p>${esc(t('ui.109'))}</p></div><div><div class="step-label">`,
    `${esc(t('ui.110'))}</div><h3>${esc(t('ui.111'))}`,
    `</h3><p>${esc(t('ui.112'))}</p></div></div></div>`,
  ].join('');
}
// Clips the export page can handle in either mode: original-file extraction
// or a bake that is already ready.
function exportableClips() {
  return clips().filter((c) => canExtract(c) || ready(c) === 'ready');
}
function exportAllBar() {
  const count = exportableClips().length;
  return [
    `<div class="export-all">`,
    button(t('ui.545', { count }), 'exportAll', 'upload', 'primary', count ? '' : 'disabled aria-describedby="export-all-reason"'),
    count ? '' : `<span class="small muted" id="export-all-reason">${esc(t('ui.546'))}</span>`,
    `</div>`,
  ].join('');
}
// Cleanup and similar warnings repeat once per file. Lines that differ only
// in their paths become one row with a count; the full lines stay expandable.
function groupedWarnings() {
  const groups = new Map();
  for (const line of sessionWarnings()) {
    const key = String(line).replace(/(?:[A-Za-z]:[\\/]|\\\\|\/)[^\s'"]*/g, '…');
    if (!groups.has(key)) groups.set(key, []);
    groups.get(key).push(line);
  }
  return [...groups].map(([summary, lines]) => ({ summary, lines }));
}
function warningItem(group) {
  if (group.lines.length === 1) return `<li>${esc(group.lines[0])}</li>`;
  return [
    `<li><details class="warning-group"><summary>${esc(group.summary)}`,
    ` <span class="warning-count">${esc(t('ui.547', { count: group.lines.length }))}</span></summary>`,
    `<ul>${group.lines.map((line) => `<li>${esc(line)}</li>`).join('')}</ul>`,
    `</details></li>`,
  ].join('');
}
function analysis() {
  const cs = clips(),
    scan = native.scan || {},
    working = ['running', 'scanning', 'analyzing'].includes(scan.state);
  return [
    `${heading(t('ui.085'), t('ui.113'), t('ui.114'))}`,
    `${working ? [
      `<div class="progress-box"><div class="progress-heading">`,
      `<h3>${esc(t('ui.115'))}</h3><span class="spinner">`,
      `</span></div><div class="progress-track"><div class="progress-bar indeterminate">`,
      `</div></div><p>${esc(scan.path || t('ui.116'))}</p>`,
      `</div>`,
    ].join('') : ''}`,
    `<div class="stats">`,
    `${[
      [t('ui.117'), cs.length, 'file', '', ''],
      [t('ui.118'), cs.filter(canExtract).length, 'download', '', ''],
      [t('ui.119'), cs.filter((c) => ready(c) === 'ready').length, 'checkCircle', 'ready', 'ready'],
      [
        t('ui.120'),
        cs.filter((c) => ready(c) !== 'ready').length,
        'link',
        'connection',
        'connection',
      ],
    ]
      .map(
        ([l, n, i, v]) =>
          [
            `<button class="stat" data-action="analysisFilter" data-value="${v}">`,
            `<span class="stat-label">${icon(i)}${esc(l)}</span>`,
            `<strong>${formatNumber(n)}</strong><small>${esc(t('ui.121'))}`,
            `</small></button>`,
          ].join(''),
      )
      .join(
        '',
      )}`,
    `</div>${exportAllBar()}<div class="two-column"><section class="panel">`,
    `<h2>${esc(t('ui.122'))}</h2>${notice(t('ui.123'), t('ui.124'), 'download')}`,
    `<div class="divider"></div>${notice(t('ui.125'), t('ui.126'), 'bone')}`,
    `</section><section class="panel"><h2>${esc(t('ui.127'))}`,
    `</h2>`,
    `${
    sessionWarnings().length
      ? [
        `<ul class="warning-list">`,
        `${groupedWarnings().map(warningItem).join('')}`,
        `</ul>`,
      ].join('')
      : `<p class="small muted">${esc(cs.length ? t('ui.128') : t('ui.129'))}</p>`
    }`,
    `<div class="divider"></div><p class="small">${esc(t('ui.130'))}`,
    `<br>${esc(t('ui.131'))}</p></section></div><div class="action-footer">`,
    `${button(t('ui.132'), 'openFiles', 'plus')}${button(t('ui.133'), 'toLibrary', 'arrow', 'primary', !cs.length ? 'disabled' : '')}`,
    `</div>`,
  ].join('');
}
function filters() {
  return [
    `<div class="filter-panel"><div><label>${esc(t('ui.134'))}`,
    `</label><div class="chips">`,
    `${[
      ['fbx', 'FBX'],
      ['generic', t('ui.035')],
      ['humanoid', t('ui.034')],
      ['baked', t('ui.036')],
    ]
      .map(
        ([v, t]) =>
          [
            `<button class="chip ${view.type.includes(v) ? 'active' : ''}" data-action="filterType" data-value="${v}" aria-pressed="${view.type.includes(v)}">`,
            `${esc(t)}</button>`,
          ].join(''),
      )
      .join('')}`,
    `</div></div><div><label>${esc(t('ui.135'))}</label>`,
    `<div class="chips">`,
    `${[
    ['generic', t('ui.035')],
    ['humanoid', t('ui.034')],
    ['skeletal', t('ui.136')],
    ['unknown', t('ui.002')],
    ]
    .map(
      ([v, t]) =>
        [
          `<button class="chip ${view.rig.includes(v) ? 'active' : ''}" data-action="filterRig" data-value="${v}" aria-pressed="${view.rig.includes(v)}">`,
          `${esc(t)}</button>`,
        ].join(''),
    )
    .join('')}`,
    `</div></div><div><label>${esc(t('ui.137'))}</label>`,
    `<div class="chips">`,
    `${[
    ['ready', t('ui.138')],
    ['connection', t('ui.139')],
    ['blocked', t('ui.037')],
    ]
    .map(
      ([v, t]) =>
        [
          `<button class="chip ${view.ready.includes(v) ? 'active' : ''}" data-action="filterReady" data-value="${v}" aria-pressed="${view.ready.includes(v)}">`,
          `${esc(t)}</button>`,
        ].join(''),
    )
    .join(
      '',
    )}`,
    `</div></div><div><label for="min-duration">${esc(t('ui.140'))}`,
    `</label><div class="range"><input id="min-duration" type="number" min="0" step="0.1" value="${esc(view.min)}" placeholder="${esc(t('ui.141'))}" aria-label="${esc(t('ui.142'))}">`,
    `<span>~</span><input id="max-duration" type="number" min="0" step="0.1" value="${esc(view.max)}" placeholder="${esc(t('ui.143'))}" aria-label="${esc(t('ui.144'))}">`,
    `</div><small class="muted">${esc(t('ui.145'))}</small>`,
    `</div><button class="link-button" data-action="resetFilters">`,
    `${esc(t('ui.146'))}</button></div>`,
  ].join('');
}
function check(c) {
  return [
    `<button class="check" data-action="check" data-id="${esc(c.id)}" role="checkbox" aria-checked="${selected.has(c.id)}" aria-label="${esc(t('ui.458', { name: c.name }))}">`,
    `${selected.has(c.id) ? icon('check') : ''}</button>`,
  ].join('');
}
// One calm progress bar for all background thumbnails instead of many fast spinners.
function previewProgress() {
  const { done, total } = previewQueue.progress();
  if (previewQueue.suspended || !total || done >= total) return '';
  const percent = Math.round((done / total) * 100);
  return [
    `<div class="preview-progress" role="progressbar" aria-valuemin="0" aria-valuemax="100" aria-valuenow="${percent}">`,
    `<div class="progress-heading"><span class="small muted">`,
    `${esc(t('ui.475', { done, total }))}</span><strong>`,
    `${percent}%</strong></div><div class="progress-track">`,
    `<div class="progress-bar" style="width:${percent}%">`,
    `</div></div></div>`,
  ].join('');
}
function thumbnail(c) {
  const p = previews.get(c.id);
  const loading = !previewQueue.suspended && previewQueue.status(c.id);
  return [
    `<div class="thumb" ${loading ? 'aria-busy="true"' : ''}>`,
    `${p?.status === 'ready' ? `<canvas data-thumbnail="${esc(c.id)}" aria-label="${esc(t('ui.459', { name: c.name }))}"></canvas>` : [
      `${loading ? '<span class="spinner"></span>' : icon(c.kind === 'fbx' ? 'cube' : c.kind === 'anim' ? 'person' : 'file', 'asset-icon')}`,
      `<span class="thumb-label">${esc(loading ? t('ui.182') : p?.reason || (typeOf(c) === 'humanoid' ? t('ui.149') : t('ui.150')))}`,
      `</span>`,
    ].join('')}`,
    `</div>`,
  ].join('');
}
function library() {
  const all = clips(),
    a = visibleClips(),
    hidden = selectedClips().filter((c) => !a.some((x) => x.id === c.id)).length;
  const root = currentRoot();
  const folderCards = (
    root
      ? rootFolders(root)
          .filter((f) => f.split('/').slice(0, -1).join('/') === view.folder)
          .map((f) => ({ source: root.path, folder: f, label: basename(f) }))
      : sourceRoots()
          .filter((r) => r.clips.length)
          .map((r) => ({ source: r.path, folder: '', label: r.name }))
  ).slice(0, 5);
  return [
    `${heading(t('ui.151'), all.length ? t('ui.152', { value0: native.packages?.length || 1, value1: all.length }) : t('ui.153'))}`,
    `<div class="toolbar"><div class="search">${icon('search')}`,
    `<label class="sr-only" for="clip-search">${esc(t('ui.154'))}`,
    `</label><input id="clip-search" type="text" placeholder="${esc(t('ui.155'))}" value="${esc(view.search)}">`,
    `</div><button class="btn filter-toggle ${view.filters ? 'active' : ''}" data-action="toggleFilters" aria-expanded="${view.filters}">`,
    `${icon('filter')}${esc(t('ui.156'))}</button></div>`,
    `${view.filters ? filters() : ''}`,
    `${!view.search && !view.type.length && !view.ready.length && folderCards.length ? [
      `<div class="folder-grid">`,
      `${folderCards.map((f) => [
        `<button class="folder-card" data-action="openSource" data-source="${esc(f.source)}" data-value="${esc(f.folder)}">`,
        `${folderArt()}<span>${esc(f.label)}</span><small>${esc(t('ui.157'))}`,
        `</small></button>`,
      ].join('')).join('')}`,
      `</div>`,
    ].join('') : ''}`,
    `<div class="subtoolbar"><strong>${esc(t('ui.158'))}`,
    `</strong><span>${esc(t('ui.159', { count: a.length }))}`,
    `</span><span class="push"></span><div class="segmented" role="group" aria-label="${esc(t('ui.160'))}">`,
    `<button class="${view.layout === 'grid' ? 'active' : ''}" data-action="layout" data-value="grid" aria-label="${esc(t('ui.161'))}">`,
    `${icon('grid')}</button><button class="${view.layout === 'list' ? 'active' : ''}" data-action="layout" data-value="list" aria-label="${esc(t('ui.162'))}">`,
    `${icon('list')}</button></div><label class="sr-only" for="sort">`,
    `${esc(t('ui.163'))}</label><select id="sort"><option value="name" ${view.sort === 'name' ? 'selected' : ''}>`,
    `${esc(t('ui.164'))}</option><option value="type" ${view.sort === 'type' ? 'selected' : ''}>`,
    `${esc(t('ui.165'))}</option><option value="duration" ${view.sort === 'duration' ? 'selected' : ''}>`,
    `${esc(t('ui.166'))}</option></select></div>${previewProgress()}`,
    `${a.length ? (view.layout === 'grid' ? [
      `<div class="clip-grid" role="list">`,
      `${a.map((c) => [
        `<article class="clip-card ${selected.has(c.id) ? 'checked' : ''} ${view.active === c.id ? 'active' : ''}" data-clip="${esc(c.id)}" tabindex="0" role="listitem" aria-label="${esc(c.name)}, ${esc(typeLabel(c))}, ${esc(readyLabel(c))}">`,
        `${check(c)}${thumbnail(c)}<div class="clip-info">`,
        `<div class="clip-name" title="${esc(c.name)}">${esc(c.name)}`,
        `</div><div class="badges">${badge(typeLabel(c))}${badge(readyLabel(c), ready(c) === 'ready' ? 'checkCircle' : 'info', 'outline')}`,
        `</div></div></article>`,
      ].join('')).join('')}`,
      `</div>`,
    ].join('') : [
      `<div class="table-wrap"><table class="clip-table">`,
      `<thead><tr><th>${esc(t('ui.167'))}</th><th>${esc(t('ui.168'))}`,
      `</th><th>${esc(t('ui.169'))}</th><th>${esc(t('ui.170'))}`,
      `</th><th>${esc(t('ui.171'))}</th></tr></thead><tbody>`,
      `${a.map((c) => [
        `<tr data-clip="${esc(c.id)}" tabindex="0" class="${view.active === c.id ? 'active' : ''}">`,
        `<td>${check(c)}</td><td class="truncate" title="${esc(c.virtual_path)}">`,
        `${esc(c.name)}</td><td>${badge(typeLabel(c))}</td>`,
        `<td>${esc(seconds(c.duration))}</td><td>${badge(readyLabel(c))}`,
        `</td></tr>`,
      ].join('')).join('')}`,
      `</tbody></table></div>`,
    ].join('')) : all.length ? empty(t('ui.172'), t('ui.173'), 'resetFilters', t('ui.146'), 'search') : empty(t('ui.174'), t('ui.175'), 'openFiles')}`,
    `<div class="selection-footer"><button class="link-button" data-action="selectVisible">`,
    `${esc(t('ui.176'))}</button><button class="link-button" data-action="selectNone">`,
    `${esc(t('ui.177'))}</button><span>${esc(t('ui.178', { count: selected.size }))}${hidden ? `<span class="selection-note">${esc(t('ui.179', { count: hidden }))}</span>` : ''}`,
    `</span>${button(t('ui.181'), 'toExport', 'upload', 'primary', !selected.size ? 'disabled' : '')}`,
    `</div>`,
  ].join('');
}
function previewPlaceholder(c, mini = false) {
  const p = c && previews.get(c.id);
  const loading = !previewQueue.suspended && !!previewQueue.status(c?.id);
  return [
    `<div class="preview-error">${loading ? '<span class="spinner"></span>' : icon(c ? 'bone' : 'eye')}`,
    `<h3>${esc(loading ? t('ui.182') : !c ? t('ui.183') : p?.status === 'blocked' ? t('ui.184') : t('ui.185'))}`,
    `</h3><p>${esc(loading ? t('ui.186') : p?.reason || (!c ? t('ui.187') : t('ui.188')))}`,
    `</p>${c && !loading ? button(p?.status === 'blocked' ? t('ui.189') : t('ui.190'), 'loadPreview', 'play', mini ? '' : 'primary') : ''}${c && !loading && typeOf(c) === 'humanoid' && !(draft?.skeleton || native.settings.skeleton) ? button(t('ui.466'), 'pickMannyEditor', 'folder') : ''}`,
    `</div>`,
  ].join('');
}
// Clip facts shown in the library's right column and below the preview player.
function clipFacts(c) {
  return [
    `<section class="clip-facts"><h3>${esc(t('ui.061'))}</h3><dl><div class="kv">`,
    `<dt>${esc(t('ui.196'))}</dt><dd>${esc(typeLabel(c))}`,
    `</dd></div><div class="kv"><dt>${esc(t('ui.197'))}`,
    `</dt><dd>${esc(seconds(c.duration || previews.get(c.id)?.duration))}`,
    `</dd></div><div class="kv"><dt>${esc(t('ui.198'))}`,
    `</dt><dd>${c.fps || previews.get(c.id)?.fps ? `${formatNumber(c.fps || previews.get(c.id).fps, { maximumFractionDigits: 2 })} FPS` : t('ui.002')}`,
    `</dd></div><div class="kv"><dt>${esc(t('ui.118'))}`,
    `</dt><dd>${esc(canExtract(c) ? t('ui.199') : t('ui.200'))}`,
    `</dd></div><div class="kv"><dt>${esc(t('ui.194'))}`,
    `</dt><dd>${esc(previews.get(c.id)?.status === 'ready' ? t('ui.201') : t('ui.202'))}`,
    `</dd></div><div class="kv"><dt>${esc(t('ui.203'))}`,
    `</dt><dd>${esc(readyLabel(c))}</dd></div></dl><p class="full-path">`,
    `${esc(c.virtual_path || c.source)}</p><button class="link-button" data-action="copyPath">`,
    `${icon('copy')}${esc(t('ui.204'))}</button></section>`,
  ].join('');
}
// The "connection needed" notice and the Unreal-specific controls for a clip.
function clipNotices(c) {
  return [
    c.kind === 'uasset' ? unrealAssetPanel(c) : ready(c) !== 'ready' ? notice(t('ui.205'), c.note || t('ui.206'), 'link') : '',
    c.route === 'unreal-export' ? button(t('ui.484'), 'revealClipFile', 'folderOpen', 'full') : '',
  ].join('');
}
function detail() {
  const c = active();
  return [
    `<aside class="detail ${view.drawer ? 'open' : ''}" aria-label="${esc(t('ui.191'))}">`,
    `<button class="icon-button drawer-close" data-action="toggleDetail" aria-label="${esc(t('ui.192'))}">`,
    `${icon('close')}</button><h2>${esc(t('ui.194'))}`,
    `</h2>${c ? `<div class="detail-name">${esc(c.name)}</div><div class="badges">${badge(typeLabel(c))}${badge(readyLabel(c), '', 'outline')}</div>` : ''}`,
    `<div class="preview-mini">${c && previews.get(c.id)?.status === 'ready' ? `<canvas data-viewer="${esc(c.id)}" aria-label="${esc(t('ui.195'))}"></canvas>` : previewPlaceholder(c, true)}`,
    `</div>`,
    `${c ? [
      clipFacts(c),
      clipNotices(c),
      `<div class="button-row">${button(t('ui.207'), 'toAvatar', 'bone', 'full')}${button(t('ui.208'), 'openPreview', 'expand', 'full')}`,
      `</div>${button(t('ui.181'), 'toExport', 'upload', 'primary full', !selected.size ? 'disabled' : '')}`,
    ].join('') : ''}`,
    `</aside>`,
  ].join('');
}
// The preview page has no right column (the viewer takes the full width), so the same
// clip information sits below the player controls instead; the large viewer replaces the thumbnail.
function previewInfo(c) {
  return [
    `<section class="clip-info" aria-label="${esc(t('ui.193'))}">`,
    `<div class="clip-info-head"><h2>${esc(t('ui.193'))}</h2>`,
    `<div class="badges">${badge(typeLabel(c))}${badge(readyLabel(c), '', 'outline')}</div></div>`,
    clipFacts(c),
    `<div class="clip-info-notices">${clipNotices(c)}</div>`,
    `<div class="clip-info-actions">${button(t('ui.207'), 'toAvatar', 'bone')}`,
    `${button(t('ui.181'), 'toExport', 'upload', 'primary', !selected.size ? 'disabled' : '')}</div>`,
    `</section>`,
  ].join('');
}
// Raw Unreal assets are converted to FBX by a headless Unreal Editor run.
function unrealAssetPanel(c) {
  const meta = c.metadata || {};
  if (!meta.unrealConvertible) return notice(t('ui.476'), t('ui.481'), 'alert');
  const converting = view.convertingClip === c.id;
  const parts = [notice(t('ui.476'), t('ui.477', { project: meta.unrealProject || '' }), 'info')];
  if (!native.settings.unreal_editor) {
    parts.push(`<p class="hint">${esc(t('ui.479'))}</p>`, button(t('ui.480'), 'chooseUnrealEditor', 'file', 'full'));
  } else {
    const disabled = converting || native.busy ? 'disabled' : '';
    parts.push(button(converting ? t('ui.482') : t('ui.478'), 'convertUnrealAsset', 'transfer', 'primary full', disabled));
  }
  return parts.join('');
}
function preview() {
  const c = active();
  if (!c) return empty(t('ui.209'), t('ui.210'), 'toLibrary', t('ui.133'), 'eye');
  const p = previews.get(c.id),
    loaded = p?.status === 'ready';
  return [
    `${heading(t('ui.211'), c.name)}<div class="player">`,
    `${loaded ? [
      `<canvas data-viewer="${esc(c.id)}" id="main-player" aria-label="${esc(t('ui.461', { name: c.name }))}">`,
      `</canvas><span class="player-caption">${p.default_character ? esc(p.default_character.name) + ' · ' : ''}${esc(p.mesh ? t('ui.213') : t('ui.214'))}`,
      `</span>`,
    ].join('') : previewPlaceholder(c)}`,
    `</div><div class="player-controls"><button class="icon-button" data-action="play" aria-label="${esc(play.running ? t('ui.215') : t('ui.216'))}" ${!loaded ? 'disabled' : ''}>`,
    `${icon(play.running ? 'pause' : 'play')}</button>`,
    `<button class="icon-button" data-action="loop" aria-label="${esc(t('ui.217'))}" aria-pressed="${play.loop}" ${!loaded ? 'disabled' : ''}>`,
    `${icon('repeat')}</button><input type="range" id="seek" min="0" max="${p?.duration || 1}" step="0.001" value="${play.time}" aria-label="${esc(t('ui.218'))}" ${!loaded ? 'disabled' : ''}>`,
    `<span class="time" id="play-time">${clock(play.time)} / ${clock(p?.duration)}`,
    `</span><select id="speed" aria-label="${esc(t('ui.219'))}">`,
    `${[0.25, 0.5, 1, 1.5, 2].map((n) => `<option value="${n}" ${n === play.speed ? 'selected' : ''}>${formatNumber(n)}×</option>`).join('')}`,
    `</select><button class="icon-button" data-action="resetCamera" aria-label="${esc(t('ui.220'))}" ${!loaded ? 'disabled' : ''}>`,
    `${icon('reset')}</button></div><div class="player-options">`,
    `<label><input type="checkbox" id="show-floor" ${play.floor ? 'checked' : ''}>`,
    `${esc(t('ui.221'))}</label><label><input type="checkbox" id="show-bones" ${showBones(p) ? 'checked' : ''}>`,
    `${esc(t('ui.222'))}</label><span class="muted">${esc(t('ui.223', { count: p?.frames?.length || 0 }))}${p?.fps ? formatNumber(p.fps) : '—'} FPS</span>`,
    `</div>${previewInfo(c)}<div class="footer-note">${esc(t('ui.224'))}`,
    `</div><div class="action-footer">${button(t('ui.225'), 'toLibrary', 'back')}`,
    `</div>`,
  ].join('');
}
function mappingEditor(c, sk) {
  const paths = [
      ...new Set([
        ...(sk.mapping || []).map((x) => x.source),
        ...Object.keys(c?.metadata?.paths || c?.paths || {}),
      ]),
    ],
    bones = sk.bones || [];
  if (!paths.length || !bones.length) return '';
  return [
    `<div class="divider"></div><h3>${esc(t('ui.227'))}`,
    `</h3><p class="hint">${esc(t('ui.228'))}</p><div class="mapping-list">`,
    `${paths.map((path) => [
      `<div class="field"><label title="${esc(path)}">${esc(path)}`,
      `</label><select data-mapping-source="${esc(path)}" aria-label="${esc(t('ui.229', { name: path }))}">`,
      `<option value="">${esc(t('ui.230'))}</option>`,
      `${bones.map((b) => [
        `<option value="${esc(b.path || b.name)}" ${draft.boneMap?.[path] === (b.path || b.name) ? 'selected' : ''}>`,
        `${esc(b.path || b.name)}</option>`,
      ].join('')).join('')}`,
      `</select></div>`,
    ].join('')).join('')}`,
    `</div>`,
  ].join('');
}
function avatar() {
  const c = active(),
    sk = native.skeleton || {},
    bones = sk.bones || sk.result?.bones || [],
    valid =
      sk.state === 'valid' &&
      sk.valid === true &&
      !view.validating &&
      String(sk.path || sk.model || '')
        .replace(/\\/g, '/')
        .toLowerCase() ===
        String(draft.skeleton || '')
          .replace(/\\/g, '/')
          .toLowerCase() &&
      (!view.active || sk.clipId === view.active) &&
      mappingKey(sk.validationMapping) === mappingKey(draft.boneMap);
  return [
    `${heading(t('ui.231'), c ? t('ui.232', { value0: c.name }) : t('ui.233'))}`,
    `<div class="two-column"><section class="panel"><h2>`,
    `${esc(t('ui.234'))}</h2><div class="form-grid">${pickerField(t('ui.235'), 'avatar-model', draft?.skeleton || native.settings.skeleton || '', 'pickFile', t('ui.236'))}`,
    `<div class="field"><label for="package-model">${esc(t('ui.237'))}`,
    `</label><select id="package-model"><option value="">`,
    `${esc(t('ui.238'))}</option>`,
    `${clips()
      .filter((c) => c.kind === 'fbx')
      .map((c) => `<option value="${esc(c.source)}">${esc(c.name)} · ${esc(c.virtual_path)}</option>`)
      .join(
        '',
      )}`,
    `</select></div>${notice(t('ui.239'), t('ui.240'), 'bone')}`,
    `<div class="button-row">${button(view.validating ? t('ui.241') : t('ui.242'), 'validateSkeleton', 'checkCircle', 'primary', view.validating ? 'disabled' : '')}${button(t('ui.243'), 'resetMapping', 'reset')}`,
    `</div></div><div class="status-line">${view.validating ? '<span class="spinner"></span>' : icon(valid ? 'checkCircle' : 'info')}${esc(valid ? t('ui.244') : t('ui.245'))}`,
    `</div>${sk.message ? notice(t('ui.246'), messageText(sk)) : ''}`,
    `${sk.validation_scope ? [
      `<dl class="validation-scope"><div class="kv"><dt>`,
      `${esc(t('ui.247'))}</dt><dd>${esc({ 'skeleton-structure': t('ui.248'), 'clip-paths-and-skeleton': t('ui.249'), 'unity-humanoid-avatar': 'Unity Humanoid Avatar', 'unity-animation-and-model': t('ui.250') }[sk.validation_scope] || sk.validation_scope)}`,
      `</dd></div>`,
      `${sk.avatar ? [
        `<div class="kv"><dt>Unity Avatar</dt><dd>${esc(sk.avatar.is_valid ? t('ui.251') : t('ui.252'))} · ${esc(sk.avatar.is_human ? t('ui.034') : t('ui.253'))}`,
        `</dd></div>`,
      ].join('') : ''}`,
      `</dl>`,
    ].join('') : ''}`,
    `${sk.missing?.length ? notice(t('ui.254'), sk.missing.join(', '), 'alert') : ''}${sk.duplicates?.length ? notice(t('ui.255'), sk.duplicates.join(', '), 'alert') : ''}${sk.errors?.length ? `<ul class="warning-list">${sk.errors.map((e) => `<li>${esc(e)}</li>`).join('')}</ul>` : ''}`,
    `</section><section class="panel"><div class="section-title">`,
    `<h2>${esc(t('ui.256'))}</h2><span class="badge">`,
    `${esc(t('ui.257', { count: bones.length }))}</span>`,
    `</div>`,
    `${bones.length ? [
      `<div class="bones-list">${bones.map((b, i) => `<div class="bone"><span>${icon('bone')} ${esc(b.name || b)}</span><small>${esc(b.parent_name ?? b.parent ?? t('ui.258'))}</small></div>`).join('')}`,
      `</div>`,
    ].join('') : empty(t('ui.259'), t('ui.260'), '', '', 'bone')}`,
    `${mappingEditor(c, sk)}<div class="divider"></div>`,
    `${notice(t('ui.261'), t('ui.262'), 'info')}${button(t('ui.263'), 'toUnity', 'link', 'full')}`,
    `</section></div><div class="action-footer">${button(t('ui.264'), 'return', 'back')}${button(t('ui.265'), 'saveSkeleton', 'save', 'primary', !valid ? 'disabled' : '')}`,
    `</div>`,
  ].join('');
}
function unity() {
  const u = native.unity || {},
    connected = u.state === 'connected' && u.connected !== false && !view.connecting;
  return [
    `${heading(t('ui.074'), t('ui.266'))}<section class="panel connection-card">`,
    `<div class="connection-art">${icon('monitor')}<span class="connector">`,
    `</span>${folderArt()}</div><h2>${esc(t('ui.267'))}`,
    `</h2><p>${esc(t('ui.268'))}</p><div class="form-grid">`,
    `${pickerField(t('ui.269'), 'unity-editor', native.settings.unityEditor || '', 'pickFile', autoDetectedHint(native.unityAuto, native.settings.unityEditor, 'ui.474'))}${pickerField(t('ui.468'), 'unreal-editor', native.settings.unreal_editor || '', 'pickFile', autoDetectedHint(native.unrealAuto, native.settings.unreal_editor, 'ui.520'))}`,
    `<p class="hint">${esc(t('ui.467'))}</p><div class="field">`,
    `<label for="unity-project">${esc(t('ui.270'))}</label>`,
    `<input id="unity-project" value="${esc(native.settings.unityProject || '')}" readonly placeholder="${esc(t('ui.271'))}">`,
    `<span class="hint">${esc(t('ui.272'))}</span></div>`,
    `<div class="kv"><dt>${esc(t('ui.273'))}</dt><dd>`,
    `${esc(u.version || u.unity_version || t('ui.274'))}`,
    `</dd></div><div class="status-line">${view.connecting ? '<span class="spinner"></span>' : icon(connected ? 'checkCircle' : 'info')}${esc(view.connecting ? t('ui.275') : connected ? t('ui.276') : t('ui.277'))}`,
    `</div></div><div class="connection-steps"><div><span class="step-number">1</span>`,
    `<b>${esc(t('ui.278'))}</b><p>${esc(t('ui.279'))}`,
    `</p></div><div><span class="step-number">2</span>`,
    `<b>${esc(t('ui.280'))}</b><p>${esc(t('ui.281'))}`,
    `</p></div><div><span class="step-number">3</span>`,
    `<b>${esc(t('ui.282'))}</b><p>${esc(t('ui.283'))}`,
    `</p></div></div><div class="button-row" style="justify-content:center">`,
    `${button(view.connecting ? t('ui.284') : t('ui.285'), 'connectUnity', 'play', 'primary', view.connecting ? 'disabled' : '')}${button(t('ui.286'), 'return')}`,
    `</div>${u.message ? `<div style="margin-top:18px">${notice(t('ui.287'), messageText(u))}</div>` : ''}`,
    `</section><div style="margin-top:18px">${notice(t('ui.288'), t('ui.289'), 'lock')}`,
    `</div>`,
  ].join('');
}
function exportPage() {
  const cs = selectedClips(),
    extract = draft.mode === 'extractOriginal';
  return [
    `${heading(t('ui.088'), t('ui.290'))}<div class="segmented" role="group" aria-label="${esc(t('ui.291'))}" style="width:max-content;margin-bottom:22px">`,
    `<button class="${extract ? 'active' : ''}" data-action="exportMode" data-value="extractOriginal">`,
    `${icon('download')}${esc(t('ui.292'))}</button><button class="${!extract ? 'active' : ''}" data-action="exportMode" data-value="bake">`,
    `${icon('bone')}${esc(t('ui.293'))}</button></div>`,
    `<div class="two-column"><section class="panel"><div class="section-title">`,
    `<h2>${esc(t('ui.294'))}</h2><span class="muted small">`,
    `${esc(t('ui.295', { count: cs.length }))}</span>`,
    `</div><div class="export-list">`,
    `${cs.map((c) => [
      `<div class="export-item">${check(c)}<div class="item-icon">`,
      `${icon(c.kind === 'fbx' ? 'cube' : 'person')}</div>`,
      `<div class="item-info"><h3 title="${esc(c.name)}">`,
      `${esc(c.name)}</h3><small>${esc(typeLabel(c))} · ${esc(extract ? (canExtract(c) ? t('ui.296') : t('ui.297')) : readyLabel(c))}`,
      `</small></div>${icon((extract && canExtract(c)) || (!extract && ready(c) === 'ready') ? 'checkCircle' : 'info')}`,
      `</div>`,
    ].join('')).join('') || `<p class="muted small">${esc(t('ui.298'))}</p>`}`,
    `</div><div style="margin-top:18px">${notice(extract ? t('ui.299') : t('ui.300'), extract ? t('ui.301') : t('ui.302'))}`,
    `</div></section><section class="panel"><h2>${esc(t('ui.303'))}`,
    `</h2><div class="form-grid">${pickerField(t('ui.304'), 'export-output', draft.outputRoot)}`,
    `<div class="field"><label for="export-profile">${esc(t('ui.305'))}`,
    `</label><select id="export-profile"><option value="unreal" ${draft.targetProfile === 'unreal' ? 'selected' : ''}>Unreal Engine</option>`,
    `<option value="unity" ${draft.targetProfile === 'unity' ? 'selected' : ''}>Unity</option>`,
    `${extract ? `<option value="original" ${draft.targetProfile === 'original' ? 'selected' : ''}>${esc(t('ui.306'))}</option>` : ''}`,
    `</select><span class="hint">${esc(t('ui.307'))}</span>`,
    `</div>`,
    `${
      !extract
        ? [
          `${pickerField(t('ui.308'), 'export-skeleton', draft.skeleton, 'pickFile')}`,
          `<div class="field"><label for="export-fps">${esc(t('ui.309'))}`,
          `</label><select id="export-fps">`,
          `${[
              [0, t('ui.310')],
              [24, `${formatNumber(24)} FPS`],
              [29.97, `${formatNumber(29.97)} FPS`],
              [30, `${formatNumber(30)} FPS`],
              [60, `${formatNumber(60)} FPS`],
            ]
              .map(
                ([n, l]) =>
                  `<option value="${n}" ${+draft.sampleRate === n ? 'selected' : ''}>${esc(l)}</option>`,
              )
              .join(
                '',
              )}`,
          `</select></div><label class="checkbox-label"><input type="checkbox" checked disabled>`,
          `${esc(t('ui.311'))}</label><div class="field"><label>`,
          `${esc(t('ui.312'))}</label><div class="radio-row">`,
          `<label><input type="radio" name="rootmotion" value="preserve" checked>`,
          `${esc(t('ui.310'))}</label><label title="${esc(t('ui.313'))}">`,
          `<input type="radio" disabled>${esc(t('ui.314'))}`,
          `</label></div></div>`,
        ].join('')
        : `<div class="notice">${icon('file')}<div><strong>${esc(t('ui.315'))}</strong><p>${esc(t('ui.316'))}</p></div></div>`
    }`,
    `${!extract ? [
      `<details class="engine-options"><summary>${esc(t('ui.317'))}`,
      `</summary><p class="hint">${esc(t('ui.318'))}</p>`,
      `<div class="form-grid">${pickerField(t('ui.319'), 'export-editor', draft.targetEditor, 'pickFile')}${pickerField(draft.targetProfile === 'unity' ? t('ui.320') : t('ui.321'), 'export-project', draft.targetProject, draft.targetProfile === 'unity' ? 'pickFolder' : 'pickFile')}`,
      `${draft.targetProfile !== 'unity' ? [
        `<div class="field"><label for="export-target-skeleton">`,
        `${esc(t('ui.322'))}</label><input id="export-target-skeleton" value="${esc(draft.targetSkeleton)}" placeholder="/Game/Characters/Hero_Skeleton.Hero_Skeleton">`,
        `</div>`,
      ].join('') : ''}`,
      `<div class="field"><label for="export-target-folder">`,
      `${esc(t('ui.323'))}</label><input id="export-target-folder" value="${esc(draft.targetFolder)}" placeholder="${draft.targetProfile === 'unity' ? 'Assets/AnimBridge' : '/Game/AnimBridge'}">`,
      `</div></div></details>`,
    ].join('') : ''}`,
    `<hr class="form-divider"><div class="field"><label for="export-collision">`,
    `${esc(t('ui.324'))}</label><select id="export-collision">`,
    `${[['rename', t('ui.325')], ['skip', t('ui.326')], ['fail', t('ui.327')], ...(extract ? [['overwrite', t('ui.328')]] : [])].map(([v, l]) => `<option value="${v}" ${draft.collisionPolicy === v ? 'selected' : ''}>${esc(l)}</option>`).join('')}`,
    `</select></div></div><div class="action-footer">`,
    `${button(t('ui.329'), 'toLibrary')}${button(t('ui.330'), 'planExport', 'upload', 'primary', !cs.length || native.busy ? 'disabled' : '')}`,
    `</div></section></div><p class="footer-note">${esc(t('ui.331'))}`,
    `</p>`,
  ].join('');
}
function queue(complete = false) {
  const j = currentJob();
  if (!j)
    return `${heading(t('ui.332'), t('ui.333'))}${empty(t('ui.334'), t('ui.335'), 'toLibrary', t('ui.133'), 'transfer')}`;
  const jobFailed = ['failed', 'interrupted'].includes(j.state);
  const rows = jobResults(j),
    total = j.total || j.plan?.jobs?.length || rows.length,
    done = rows.filter(resultTerminal).length,
    success = rows.filter(good).length,
    failed = rows.filter((r) => ['error', 'blocked'].includes(r.status)).length,
    cancelled = rows.filter((r) => ['cancelled', 'canceled'].includes(r.status)).length,
    percent = total ? Math.round((done / total) * 100) : 0;
  return [
    `${heading(complete ? t('ui.336') : t('ui.332'), t('ui.337', { value0: esc(j.attemptId || j.attempt_id || jobId(j)) }))}`,
    `${
      complete
        ? [
          `<div class="panel completion"><div class="success-symbol">`,
          `${icon(failed ? 'info' : cancelled ? 'stop' : 'check')}`,
          `</div><h1>${esc(jobFailed ? t('ui.338') : failed ? (success ? t('ui.339') : t('ui.340')) : cancelled ? t('ui.341') : success ? t('ui.342') : t('ui.343'))}`,
          `</h1><p>${esc(messageText(j) || t('ui.344'))}</p>`,
          `<div class="stats" style="grid-template-columns:repeat(3,1fr)">`,
          `${[
              [t('ui.045'), success],
              [t('ui.345'), failed],
              [
                t('ui.346'),
                rows.filter((r) => ['cancelled', 'canceled', 'skipped'].includes(r.status)).length,
              ],
            ]
              .map(
                ([l, n]) =>
                  `<div class="stat"><small>${esc(l)}</small><strong>${formatNumber(n)}</strong></div>`,
              )
              .join('')}`,
          `</div></div>`,
        ].join('')
        : [
          `<div class="progress-box"><div class="progress-heading">`,
          `<h3>${esc(t('ui.347'))}</h3><div><span class="small muted">`,
          `${esc(t('ui.348', { done, total }))}</span>　<strong>`,
          `${formatNumber(percent / 100, { style: 'percent' })}`,
          `</strong></div></div><div class="progress-track">`,
          `<div class="progress-bar" style="width:${percent}%">`,
          `</div></div><p>${esc(native.busy ? t('ui.349') : t('ui.350'))}`,
          `</p></div>`,
        ].join('')
    }`,
    `<div>`,
    `${rows.map((r) => [
      `<div class="job-row"><span>${icon(good(r) ? 'checkCircle' : ['error', 'blocked'].includes(r.status) ? 'alert' : 'clock')}`,
      `</span><div class="info"><h3>${esc(r.name || r.id)}`,
      `</h3><p>${esc(messageText(r, statusText(r.step || r.status)))}`,
      `</p></div><div class="job-state">${!resultTerminal(r) ? '<span class="spinner"></span>' : ''}${esc(statusText(r.status))}`,
      `</div>`,
      `${['error', 'blocked'].includes(r.status) ? [
        `<button class="icon-button" data-action="showErrorItem" data-id="${esc(r.id)}" aria-label="${esc(t('ui.460', { name: r.name }))}">`,
        `${icon('chevron')}</button>`,
      ].join('') : ''}`,
      `</div>`,
    ].join('')).join('')}`,
    `</div><div class="action-footer">${native.busy ? button(t('ui.352'), 'cancelJob', 'close') : button(t('ui.353'), 'toLibrary', 'back')}${!native.busy && failed && j.attemptId === native.retryAttemptId ? button(t('ui.354'), 'retryFailed', 'reset') : ''}${!native.busy ? button(t('ui.355'), 'saveReport', 'save') : ''}${!native.busy ? button(t('ui.356'), 'openOutput', 'folderOpen', 'primary') : ''}`,
    `</div>${notice(t('ui.357'), t('ui.358'))} `,
  ].join('');
}
function errors() {
  const jobs = [
      ...new Map(
        [...(native.jobs || []), ...(native.history || [])].map((j) => [jobId(j), j]),
      ).values(),
    ],
    rs = jobs.flatMap((j) =>
      jobResults(j)
        .filter((r) => ['error', 'blocked', 'warning'].includes(r.status))
        .map((r) => ({ ...r, job: jobId(j) })),
    );
  return [
    `${heading(t('ui.090'), t('ui.359'))}`,
    `${rs.length ? rs.map((r) => [
      `<section class="panel"><div class="section-title">`,
      `<h3>${icon(r.status === 'warning' ? 'info' : 'alert')} ${esc(r.name)}`,
      `</h3>${badge(statusText(r.status))}</div><div class="badge-line">`,
      `${badge(r.errorCode || r.error_code || t('ui.360'))}${badge(r.step || t('ui.361'))}`,
      `</div><pre class="log">${esc(messageText(r, t('ui.362')))}`,
      `</pre>${r.recovery ? notice(t('ui.363'), r.recovery) : ''}`,
      `<div class="button-row">${button(t('ui.226'), 'toAvatar', 'bone')}${button(t('ui.364'), 'toExport', 'folder')}${r.log ? `<button class="btn" data-action="openLog" data-value="${esc(r.log)}">${icon('file')}${esc(t('ui.365'))}</button>` : ''}`,
      `<button class="btn" data-action="copyError" data-value="${esc(r.message)}">`,
      `${icon('copy')}${esc(t('ui.366'))}</button></div>`,
      `</section>`,
    ].join('')).join('') : empty(t('ui.367'), t('ui.368'), '', '', 'checkCircle')}`,
    `<p class="footer-note">${esc(t('ui.369'))}</p>`,
  ].join('');
}
function history() {
  const js = [...(native.jobs || []), ...(native.history || [])]
    .filter((j, i, a) => a.findIndex((x) => jobId(x) === jobId(j)) === i)
    .sort((a, b) => String(b.createdAt || '').localeCompare(String(a.createdAt || '')));
  return [
    `${heading(t('ui.068'), t('ui.370'))}`,
    `${js.length ? js.map((j) => [
      `<div class="history-row"><div><h3>${esc(j.name || j.mode || t('ui.371'))}`,
      `</h3><p>${esc(jobId(j))} · ${esc(date(j.createdAt || j.startedAt || j.timestamp))}`,
      `</p></div><div class="row-end">${badge(t('ui.372', { value0: jobResults(j).filter(good).length }))}`,
      `<button class="btn" data-action="viewJob" data-value="${esc(jobId(j))}">`,
      `${esc(t('ui.373'))}${icon('chevron')}</button></div>`,
      `</div>`,
    ].join('')).join('') : empty(t('ui.374'), t('ui.375'), 'toLibrary', t('ui.133'), 'clock')}`,
  ].join('');
}
function settings() {
  const s = settingsDraft || native.settings;
  return [
    `${heading(t('ui.376'), t('ui.377'))}<div class="settings-layout">`,
    `<div><section class="panel"><h2>${esc(t('ui.378'))}`,
    `</h2><div class="three-fields"><div class="field">`,
    `<label for="setting-language">${esc(t('ui.379'))}`,
    `</label><select id="setting-language">${SUPPORTED_LOCALES.map(({ code, name }) => `<option value="${code}" ${normalizeLocale(s.language) === code ? 'selected' : ''}>${esc(name)}</option>`).join('')}`,
    `</select><span class="hint">${esc(t('ui.456'))}</span>`,
    `</div><div class="field"><label for="setting-theme">`,
    `${esc(t('ui.382'))}</label><select id="setting-theme" disabled>`,
    `<option>${esc(t('ui.383'))}</option></select></div>`,
    `<div class="field"><label for="setting-scale">${esc(t('ui.384'))}`,
    `</label><select id="setting-scale">${[100, 125, 150, 200].map((n) => `<option value="${n}" ${+s.uiScale === n ? 'selected' : ''}>${formatNumber(n / 100, { style: 'percent' })}</option>`).join('')}`,
    `</select></div></div></section><section class="panel">`,
    `<h2>${esc(t('ui.385'))}</h2><div class="form-grid">`,
    `${pickerField(t('ui.386'), 'setting-output', s.outputRoot)}${pickerField(t('ui.387'), 'setting-python', s.python || 'python', 'pickFile')}${pickerField(t('ui.388'), 'setting-blender', s.blender || '', 'pickFile')}`,
    `<p class="small muted">${esc(t('ui.389'))}</p></div>`,
    `</section><section class="panel"><h2>${esc(t('ui.077'))}`,
    `</h2><div class="field"><label for="setting-cache">`,
    `${esc(t('ui.390'))}</label><input id="setting-cache" value="${esc(s.cacheRoot || '')}" readonly>`,
    `<span class="hint">${esc(t('ui.391'))}</span></div>`,
    `<p class="footer-note">${esc(t('ui.392'))}</p><div class="button-row" style="margin-top:14px">`,
    `${button(t('ui.393'), 'clearCache', 'trash', '', native.busy ? 'disabled' : '')}`,
    `</div></section></div><aside class="settings-aside">`,
    `<section class="panel local-card">${icon('lock')}`,
    `<h3>${esc(t('ui.288'))}</h3><p>${esc(t('ui.462'))}`,
    `</p></section><section class="panel local-card">`,
    `${icon('cube')}<h3>${esc(native.unity.state === 'connected' ? t('ui.396') : t('ui.397'))}`,
    `</h3><p>${esc(t('ui.398'))}</p><div style="margin-top:18px">`,
    `${button(t('ui.226'), 'toUnity', 'link')}</div></section>`,
    `</aside></div><div class="action-footer"><span class="muted small" style="margin-right:auto">`,
    `${esc(settingsDirty ? t('ui.399') : t('ui.400'))}`,
    `</span>${button(t('ui.401'), 'resetSettings', 'reset')}${button(t('ui.402'), 'saveSettings', 'save', 'primary', !settingsDirty ? 'disabled' : '')}`,
    `</div>`,
  ].join('');
}
function buildMarkup() {
  const pages = {
    home,
    analysis,
    library,
    preview,
    avatar,
    unity,
    export: exportPage,
    queue: () => queue(false),
    complete: () => queue(true),
    errors,
    history,
    settings,
  };
  const content = (pages[view.page] || home)();
  return [
    `${titlebar()}<div class="shell">${rail()}${sidebar()}`,
    `<main class="workspace">${breadcrumb()}<div class="page-layout">`,
    `<div class="page">`,
    `${view.error ? [
      `<div class="inline-error" role="alert">${icon('alert')}`,
      `<span>${esc(view.error)}</span><button class="icon-button" data-action="dismissError" aria-label="${esc(t('ui.403'))}">`,
      `${icon('close')}</button></div>`,
    ].join('') : ''}`,
    `${showReadBanner() ? [
      `<div class="read-operation" role="status"><span class="spinner">`,
      `</span><span>${esc(t('ui.404'))}</span>${button(t('ui.405'), 'cancelRead', 'close')}`,
      `</div>`,
    ].join('') : ''}`,
    `${content}</div>${view.page === 'library' ? detail() : ''}`,
    `</div></main></div>`,
  ].join('');
}
// Replacing the captured .topbar mid-drag would drop pointer capture and end the move.
let moveGesture = null;
let moveSerial = 0;
let renderDeferred = false;
function render() {
  if (moveGesture) {
    renderDeferred = true;
    return;
  }
  renderDeferred = false;
  localizeDocument();
  const markup = buildMarkup();
  // Nothing visible changed (e.g. only the host's read-busy flag toggled): keep the DOM,
  // canvases and viewers untouched instead of rebuilding the whole app.
  if (markup === lastMarkup && app.firstElementChild) return;
  lastMarkup = markup;
  const focused = document.activeElement;
  const fi = focused?.id,
    ss = focused?.selectionStart,
    se = focused?.selectionEnd;
  const scroll = $('.page')?.scrollTop || 0;
  viewers.forEach((v) => v.destroy());
  viewers = [];
  viewerObserver?.disconnect();
  // Viewers that scroll out of sight stop rendering; observers are unavailable in some test hosts.
  viewerObserver =
    typeof IntersectionObserver === 'function'
      ? new IntersectionObserver(
          (entries) => {
            for (const entry of entries) entry.target.poseViewer?.setVisible(entry.isIntersecting);
          },
          { rootMargin: '100px' },
        )
      : null;
  app.innerHTML = markup;
  document.documentElement.style.fontSize = '14px'; // Native host applies the complete WebView zoom.
  const f = fi && document.getElementById(fi);
  if (f && focused?.tagName === 'INPUT') {
    f.focus({ preventScroll: true });
    if (f.type === 'text' && ss !== null)
      try {
        f.setSelectionRange(ss, se);
      } catch {}
  }
  const page = $('.page');
  if (page) page.scrollTop = scroll;
  document.querySelectorAll('canvas[data-viewer],canvas[data-thumbnail]').forEach(mountViewer);
  ensureAnimation();
}
function mountViewer(c) {
  const p = previews.get(c.dataset.viewer || c.dataset.thumbnail);
  if (p?.status === 'ready' && p.frames?.length && p.bones?.length) {
    try {
      const v = new PoseViewer(c, p);
      c.poseViewer = v;
      viewerObserver?.observe(c);
      v.floor = play.floor;
      v.skeleton = showBones(p);
      const time = c.dataset.thumbnail ? (c.dataset.thumbnail === hoveredClip ? hoverTime : 0) : play.time;
      v.setFrame(Math.min(p.frames.length - 1, Math.floor(time * (p.fps || 30))));
      viewers.push(v);
    } catch (e) {
      c.replaceWith(
        Object.assign(document.createElement('span'), {
          textContent: t('ui.406'),
        }),
      );
    }
  }
}
// A finished background preview only changes its own card thumbnail; replace just that node.
// Returns false when anything else on screen depends on the result (caller does a full render).
function patchThumbnails(ids) {
  if (view.page !== 'library' || view.layout !== 'grid' || view.sort === 'duration' || moveGesture) return false;
  // The overall progress bar must stay in sync with the patched cards.
  const bar = app.querySelector('.preview-progress'), barMarkup = previewProgress();
  if (!bar !== !barMarkup) return false;
  const jobs = [];
  for (const id of ids) {
    if (id === view.active) return false;
    const c = clips().find((x) => x.id === id);
    const card = [...app.querySelectorAll('.clip-card')].find((x) => x.dataset.clip === id);
    const thumb = card?.querySelector('.thumb');
    if (!c || !thumb) {
      // A visible card without a thumbnail node cannot be patched; fall back to a full render.
      if (c && visibleClips().some((x) => x.id === id)) return false;
      continue;
    }
    jobs.push([thumb, c]);
  }
  for (const [thumb, c] of jobs) {
    for (const canvas of thumb.querySelectorAll('canvas')) {
      const v = canvas.poseViewer;
      if (v) {
        v.destroy();
        viewers = viewers.filter((x) => x !== v);
      }
    }
    const holder = document.createElement('div');
    holder.innerHTML = thumbnail(c);
    const next = holder.firstElementChild;
    thumb.replaceWith(next);
    next.querySelectorAll('canvas[data-thumbnail]').forEach(mountViewer);
  }
  if (bar) bar.outerHTML = barMarkup;
  lastMarkup = buildMarkup();
  ensureAnimation();
  return true;
}
function confirmDialog(title, text, actions, html = '') {
  return new Promise((resolve) => {
    if (modal.open) modal.close();
    const caller = document.activeElement;
    modal.innerHTML = [
      `<h2 id="modal-title">${esc(title)}</h2><p>${esc(text)}`,
      `</p>${html ? `<div class="modal-content">${html}</div>` : ''}`,
      `<div class="actions">${actions.map((a) => `<button class="btn ${a.primary ? 'primary' : ''}" data-answer="${esc(a.value)}">${esc(a.label)}</button>`).join('')}`,
      `</div>`,
    ].join('');
    let finished = false;
    const finish = (v) => {
      if (finished) return;
      finished = true;
      modal.close();
      modal.onclick = null;
      modal.oncancel = null;
      caller?.isConnected && caller.focus();
      resolve(v);
    };
    modal.onclick = (e) => {
      const b = e.target.closest('[data-answer]');
      if (b) finish(b.dataset.answer);
    };
    modal.oncancel = (e) => {
      e.preventDefault();
      finish('cancel');
    };
    modal.showModal();
    modal.querySelector('button')?.focus();
  });
}
async function openFiles() {
  try {
    const r = await request('chooseFiles', pickerLabels(false));
    if (r?.paths?.length) await scan(r.paths);
  } catch (e) {
    showError(e);
  }
}
async function scan(paths) {
  recordPageChange('analysis');
  view.page = 'analysis';
  view.error = '';
  native.scan = { state: 'scanning', path: paths.join(', ') };
  render();
  try {
    await request('scan', { paths });
  } catch (e) {
    native.scan.state = 'error';
    showError(e);
  }
}
async function syncSelection() {
  saveView();
  try {
    await request('select', { ids: [...selected] });
  } catch (e) {
    showError(e);
  }
}
function setSelected(id) {
  if (selected.has(id)) selected.delete(id);
  else selected.add(id);
  render();
  syncSelection();
}
async function chooseField(field, folder) {
  if (field === 'unreal-editor') return chooseMannyEditor();
  try {
    const r = await request(folder ? 'chooseFolder' : 'chooseFiles', {
      ...pickerLabels(folder),
      purpose: field,
      multiple: false,
      engine: field.startsWith('unity-') ? 'unity' : draft?.targetProfile === 'unity' ? 'unity' : 'unreal',
      targetFolder: draft?.targetFolder || (draft?.targetProfile === 'unreal' ? '/Game/AnimBridge' : 'Assets/AnimBridge'),
      filter:
        field.includes('editor') || field.includes('python') || field.includes('blender')
          ? 'exe'
          : field.includes('model') || field.includes('skeleton')
            ? 'fbx'
            : 'all',
    });
    if (r?.paths?.[0]) {
      const el = document.getElementById(field);
      if (el) {
        el.value = r.paths[0];
        if (field === 'export-project' && !draft.targetFolder) draft.targetFolder = draft.targetProfile === 'unreal' ? '/Game/AnimBridge' : 'Assets/AnimBridge';
        fieldChanged(el);
        if (['unity-editor', 'unity-project', 'avatar-model'].includes(field)) captureField(el);
      }
    }
  } catch (e) {
    showError(e);
  }
}
// Approves the clip's own .uproject through the native picker (engine project grant).
async function chooseUnrealProject(c) {
  const project = c.metadata?.unrealProject || '';
  const result = await request('chooseFiles', {
    ...pickerLabels(false),
    title: t('ui.485', { project: project.split(/[\/]/).pop() }),
    purpose: 'unreal-source-project',
    engine: 'unreal',
    multiple: false,
    filter: 'uproject',
    initialFolder: project.replace(/[\/][^\/]*$/, ''),
  });
  return result?.grants?.[0]?.grant || '';
}
async function convertUnrealAsset(c, projectGrant = '') {
  view.convertingClip = c.id;
  render();
  toast(t('ui.483'));
  try {
    const payload = projectGrant ? { clipId: c.id, unrealProjectGrant: projectGrant } : { clipId: c.id };
    const result = await request('convertUnrealAsset', payload);
    applyState(await request('getState'));
    if (result.clipIds?.[0]) view.active = result.clipIds[0];
    toast(t('ui.486', { count: result.clipIds?.length || 0 }));
  } catch (error) {
    if (error.code === 'UNREAL_PROJECT_APPROVAL_REQUIRED' && !projectGrant) {
      view.convertingClip = '';
      const grant = await chooseUnrealProject(c);
      if (grant) return convertUnrealAsset(c, grant);
      return;
    }
    showError(error);
  } finally {
    view.convertingClip = '';
    render();
  }
}
async function chooseMannyEditor() {
  try {
    const result = await request('chooseFiles', {
      ...pickerLabels(false), purpose: 'unreal-editor', engine: 'unreal', multiple: false, filter: 'exe',
    });
    const path = result?.paths?.[0];
    if (!path) return;
    await request('saveSettings', { settings: { unreal_editor: path } });
    native.settings.unreal_editor = path;
    invalidatePreviews();
    render();
  } catch (error) {
    showError(error);
  }
}
function captureField(el) {
  if (el.id === 'unity-editor') native.settings.unityEditor = el.value;
  if (el.id === 'unity-project') native.settings.unityProject = el.value;
  if (el.id === 'avatar-model' && draft.skeleton !== el.value) {
    draft.skeleton = el.value;
    native.skeleton = { state: 'missing', valid: false };
    invalidatePreviews();
    const save = document.querySelector('[data-action="saveSkeleton"]');
    if (save) save.disabled = true;
  }
}
function fieldChanged(el) {
  const v = el.value;
  const exportMap = {
    'export-output': 'outputRoot',
    'export-skeleton': 'skeleton',
    'export-profile': 'targetProfile',
    'export-fps': 'sampleRate',
    'export-collision': 'collisionPolicy',
    'export-editor': 'targetEditor',
    'export-project': 'targetProject',
    'export-target-skeleton': 'targetSkeleton',
    'export-target-folder': 'targetFolder',
  };
  if (exportMap[el.id]) {
    draft[exportMap[el.id]] = el.id === 'export-fps' ? +v : v;
    if (el.id === 'export-skeleton') invalidatePreviews();
    draft.direction = draft.targetProfile === 'unity' ? 'unreal-to-unity' : 'unity-to-unreal';
    view.plan = null;
  }
  const settingsMap = {
    'setting-language': 'language',
    'setting-output': 'outputRoot',
    'setting-python': 'python',
    'setting-blender': 'blender',
    'setting-cache': 'cacheRoot',
    'setting-scale': 'uiScale',
  };
  if (settingsMap[el.id]) {
    settingsDraft[settingsMap[el.id]] = el.id === 'setting-scale' ? +v : v;
    settingsDirty = true;
    const b = document.querySelector('[data-action="saveSettings"]');
    if (b) b.disabled = false;
  }
  captureField(el);
}
async function loadPreview() {
  const c = active();
  if (!c) return;
  syncPreviews();
  if (previews.get(c.id)?.status === 'ready') startPlay();
  else previewQueue.retry(c.id);
  render();
}
function startPlay() {
  const p = previews.get(view.active);
  if (p?.status !== 'ready') return;
  if (play.time >= p.duration) play.time = 0;
  play.running = true;
  lastTick = 0;
  ensureAnimation();
}
function stopPlay() {
  play.running = false;
  lastTick = 0;
}
function ensureAnimation() {
  if (!raf && !document.hidden && (play.running || previews.get(hoveredClip)?.status === 'ready'))
    raf = requestAnimationFrame(tick);
}
function tick(now) { // "now" is the rAF timestamp; "t" stays the i18n lookup
  raf = 0;
  if (document.hidden) {
    lastTick = 0;
    hoverTick = 0;
    return;
  }
  const p = previews.get(view.active);
  if (play.running && p?.status === 'ready') {
    if (lastTick) play.time += Math.min(0.1, (now - lastTick) / 1000) * play.speed;
    lastTick = now;
    if (play.time > p.duration) {
      if (play.loop && p.duration > 0) play.time %= p.duration;
      else {
        play.time = p.duration;
        stopPlay();
        const button = app.querySelector('[data-action="play"]');
        if (button) {
          button.innerHTML = icon('play');
          button.setAttribute('aria-label', t('ui.216'));
        }
      }
    }
    for (const v of viewers) if (v.canvas.dataset.viewer === view.active)
      v.setFrame(Math.min(p.frames.length - 1, Math.floor(play.time * (p.fps || 30))));
    if ($('#seek')) $('#seek').value = play.time;
    if ($('#play-time')) $('#play-time').textContent = `${clock(play.time)} / ${clock(p.duration)}`;
  } else if (play.running) stopPlay();
  const h = previews.get(hoveredClip);
  if (hoveredClip && h?.status === 'ready') {
    if (hoverTick) hoverTime += Math.min(0.1, (now - hoverTick) / 1000) * play.speed;
    hoverTick = now;
    hoverTime = h.duration > 0 ? hoverTime % h.duration : 0;
    for (const v of viewers) {
      const id = v.canvas.dataset.thumbnail || v.canvas.dataset.viewer;
      if (id === hoveredClip && (v.canvas.dataset.thumbnail || !play.running))
        v.setFrame(Math.min(h.frames.length - 1, Math.floor(hoverTime * (h.fps || 30))));
    }
  } else hoverTick = 0;
  ensureAnimation();
}
function hoverPreview(id) {
  if (hoveredClip === id) return;
  const previous = hoveredClip;
  hoveredClip = id;
  hoverTime = hoverTick = 0;
  clearTimeout(hoverPriorityTimer);
  hoverPriorityTimer = setTimeout(() => {
    previewQueue.priority = hoveredClip || view.active;
  }, 150);
  for (const v of viewers) if (v.canvas.dataset.thumbnail === previous) v.setFrame(0);
  ensureAnimation();
}
async function validateSkeleton() {
  const path = $('#avatar-model')?.value || draft.skeleton;
  if (!path) {
    showError(t('ui.410'));
    return;
  }
  draft.skeleton = path;
  invalidatePreviews();
  view.validating = true;
  render();
  try {
    const r = await request('validateSkeleton', {
      path,
      mapping: draft.boneMap || {},
      ...(view.active ? { clipId: view.active } : {}),
    });
    native.skeleton = {
      ...r,
      path,
      clipId: view.active,
      validationMapping: { ...(draft.boneMap || {}) },
      state: r.valid === true && r.status === 'valid' ? 'valid' : 'invalid',
      message: r.reason || r.message || '',
    };
    if (native.skeleton.state === 'valid') toast(t('ui.411'));
  } catch (e) {
    native.skeleton = { state: 'invalid', message: e.message };
  } finally {
    view.validating = false;
    render();
  }
}
async function connectUnity() {
  const editorPath = $('#unity-editor')?.value || native.settings.unityEditor,
    projectPath = $('#unity-project')?.value || native.settings.unityProject;
  if (!editorPath) {
    showError(t('ui.412'));
    return;
  }
  native.settings.unityEditor = editorPath;
  native.settings.unityProject = projectPath;
  view.connecting = true;
  render();
  try {
    const r = await request('connectUnity', { editorPath });
    native.unity = {
      ...r,
      state: r.state || (r.status === 'ok' || r.connected ? 'connected' : 'error'),
    };
    if (native.unity.state === 'connected') toast(t('ui.413'));
  } catch (e) {
    native.unity = { state: 'error', message: e.message };
  } finally {
    view.connecting = false;
    render();
  }
}
function exportOptions() {
  const options = { ...draft };
  if (options.collisionPolicy !== 'overwrite') options.explicitOverwrite = false;
  if (options.mode === 'extractOriginal')
    for (const key of [
      'sampleRate',
      'skeleton',
      'targetEditor',
      'targetProject',
      'targetSkeleton',
      'targetFolder',
      'boneMap',
    ])
      delete options[key];
  return options;
}
async function planExport() {
  if (!selected.size || native.busy) return;
  if (!draft.outputRoot) {
    showError(t('ui.414'));
    $('#export-output')?.focus();
    return;
  }
  view.error = '';
  try {
    const plan = await request('plan', {
      options: { ...exportOptions(), explicitOverwrite: false },
      clipIds: [...selected],
    });
    view.plan = plan;
    const jobs = plan.jobs || [],
      blocked = jobs.filter((j) => j.blocked_reason || j.blockedReason);
    const overwrite = draft.collisionPolicy === 'overwrite';
    const msg = t('ui.420', {
      value0: selected.size,
      value1: draft.mode === 'extractOriginal' ? t('ui.415') : t('ui.125'),
      value2: blocked.length ? t('ui.416', { value0: blocked.length }) : '',
      value3: overwrite ? t('ui.417') : t('ui.418'),
      value4: draft.mode === 'bake' && draft.targetProject ? t('ui.419') : '',
    });
    const response = await confirmDialog(
      t('ui.421'),
      msg,
      [
        { label: t('ui.422'), value: 'cancel' },
        {
          label: overwrite ? t('ui.423') : t('ui.424'),
          value: 'start',
          primary: !overwrite,
        },
      ],
      [
        `${notice(t('ui.425'), draft.outputRoot, 'folder')}`,
        `<div class="plan-targets"><h3>${esc(t('ui.426'))}`,
        `</h3>${jobs.map((j) => `<p class="full-path">${esc(j.output)}</p>`).join('')}`,
        `</div>${blocked.map((j) => notice(j.clip?.name || t('ui.427'), j.blocked_reason || j.blockedReason, 'alert')).join('')}`,
      ].join(''),
    );
    if (response === 'start') {
      draft.explicitOverwrite = overwrite;
      const result = await request('start', {
        options: exportOptions(),
        clipIds: [...selected],
        plan,
      });
      view.jobId = result?.attemptId || plan.attemptId || result?.jobId || plan.jobId || '';
      native.busy = true;
      go('queue');
      await request('getState').then(applyState);
    }
  } catch (e) {
    showError(e);
  }
}
async function saveSettings() {
  if (settingsSaving) return false;
  settingsSaving = true;
  try {
    const submitted = Object.fromEntries(
      ['language', 'theme', 'uiScale', ...(settingsDraft.outputRoot !== native.settings.outputRoot ? ['outputRoot'] : [])].map((k) => [
        k,
        settingsDraft[k],
      ]),
    );
    const result = await request('saveSettings', {
      settings: submitted,
    });
    const changedWhileSaving =
      settingsDirty && Object.keys(submitted).some((k) => settingsDraft[k] !== submitted[k]);
    native.settings = { ...native.settings, ...(result?.settings || result) };
    native.settings.language = normalizeLocale(native.settings.language);
    setLocale(native.settings.language);
    if (!changedWhileSaving) {
      settingsDraft = { ...native.settings };
      settingsDirty = false;
    }
    draft = { ...draft, python: native.settings.python, blender: native.settings.blender };
    toast(t('ui.428'));
    render();
    return !changedWhileSaving;
  } catch (e) {
    showError(e);
    return false;
  } finally {
    settingsSaving = false;
  }
}
async function action(name, el) {
  switch (name) {
    case 'showSupport':
      showSupport();
      break;
    case 'showWelcome':
      showWelcome();
      break;
    case 'openFiles':
      await openFiles();
      break;
    case 'openRecent': {
      const recent = native.packages.find((row) => row.path === el.dataset.value);
      if (recent?.approvalState === 'stale' || ['permissionDenied', 'unavailable'].includes(recent?.sourceState)) {
        if (recent.directory) {
          const result = await request('chooseFolder', pickerLabels(true));
          if (result.paths?.length) await scan(result.paths);
        } else await openFiles();
      } else await scan([el.dataset.value]);
      break;
    }
    case 'toLibrary':
      goOrBack('library');
      break;
    case 'toExport':
      if (selected.size) go('export');
      else toast(t('ui.429'));
      break;
    case 'toAvatar':
      returnPage = view.page === 'preview' ? 'preview' : 'export';
      go('avatar');
      break;
    case 'toUnity':
      returnPage = view.page;
      go('unity');
      break;
    case 'return':
      goOrBack(returnPage);
      break;
    case 'openSource': {
      const source = el.dataset.source || '';
      const root = source ? sourceRoots().find((r) => samePath(r.path, source)) : null;
      view.source = root ? root.path : '';
      view.folder = root ? el.dataset.value || '' : '';
      // A Recent root that is not loaded yet is analysed first.
      if (root && !root.clips.length) await action('openRecent', { dataset: { value: root.path } });
      else go('library');
      break;
    }
    case 'toggleTree':
      view.tree = { ...view.tree, [el.dataset.key]: el.dataset.open !== 'true' };
      saveView();
      render();
      break;
    case 'exportAll': {
      const ids = exportableClips().map((c) => c.id);
      if (!ids.length) break;
      selected = new Set(ids);
      await syncSelection();
      go('export');
      break;
    }
    case 'check':
      setSelected(el.dataset.id);
      break;
    case 'selectVisible':
      visibleClips().forEach((c) => selected.add(c.id));
      render();
      syncSelection();
      break;
    case 'selectNone':
      selected.clear();
      render();
      syncSelection();
      break;
    case 'toggleFilters':
      view.filters = !view.filters;
      render();
      break;
    case 'resetFilters':
      view.search = '';
      view.type = [];
      view.rig = [];
      view.ready = [];
      view.min = '';
      view.max = '';
      view.error = '';
      render();
      saveView();
      break;
    case 'filterType':
    case 'filterReady':
    case 'filterRig': {
      const key = name === 'filterType' ? 'type' : name === 'filterRig' ? 'rig' : 'ready',
        v = el.dataset.value;
      view[key] = view[key].includes(v) ? view[key].filter((x) => x !== v) : [...view[key], v];
      render();
      saveView();
      break;
    }
    case 'analysisFilter':
      view.ready = el.dataset.value ? [el.dataset.value] : [];
      go('library');
      break;
    case 'layout':
      view.layout = el.dataset.value;
      render();
      saveView();
      break;
    case 'toggleDetail':
      view.drawer = !view.drawer;
      render();
      break;
    case 'openPreview':
      go('preview');
      if (active() && !previews.has(view.active)) loadPreview();
      break;
    case 'loadPreview':
      await loadPreview();
      break;
    case 'play':
      if (play.running) {
        stopPlay();
        render();
      } else if (previews.get(view.active)?.status === 'ready') {
        startPlay();
        render();
      }
      break;
    case 'loop':
      play.loop = !play.loop;
      render();
      break;
    case 'resetCamera':
      viewers.forEach((v) => v.reset());
      break;
    case 'convertUnrealAsset':
      if (active()) await convertUnrealAsset(active());
      break;
    case 'chooseUnrealEditor':
      await chooseMannyEditor();
      break;
    case 'revealClipFile':
      if (active()) await request('openPath', { path: active().source });
      break;
    case 'copyPath':
      if (active()) {
        await request('copyText', { text: active().source });
        toast(t('ui.430'));
      }
      break;
    case 'pickFile':
    case 'pickFolder':
      await chooseField(el.dataset.field, name === 'pickFolder');
      break;
    case 'validateSkeleton':
      await validateSkeleton();
      break;
    case 'saveSkeleton':
      await request('saveSettings', {
        settings: { skeleton: draft.skeleton, boneMap: draft.boneMap || {} },
      });
      native.settings.skeleton = draft.skeleton;
      toast(t('ui.431'));
      goOrBack(returnPage);
      break;
    case 'resetMapping':
      native.skeleton = { ...native.skeleton, state: 'missing', valid: false };
      draft.boneMap = {};
      invalidatePreviews();
      render();
      break;
    case 'pickMannyEditor':
      await chooseMannyEditor();
      break;
    case 'connectUnity':
      await connectUnity();
      break;
    case 'exportMode':
      draft.mode = el.dataset.value;
      if (draft.mode === 'bake' && draft.collisionPolicy === 'overwrite') {
        draft.collisionPolicy = 'rename';
        draft.explicitOverwrite = false;
      }
      if (draft.mode === 'bake' && !['unity', 'unreal'].includes(draft.targetProfile))
        draft.targetProfile = 'unreal';
      view.plan = null;
      render();
      break;
    case 'planExport':
      await planExport();
      break;
    case 'cancelRead':
      previewQueue.suspended = true;
      await request('cancel');
      await previewQueue.drain();
      render();
      toast(t('ui.432'));
      break;
    case 'cancelJob':
      if (
        (await confirmDialog(t('ui.433'), t('ui.434'), [
          { label: t('ui.435'), value: 'cancel' },
          { label: t('ui.436'), value: 'yes' },
        ])) === 'yes'
      ) {
        await request('cancel', { jobId: jobId(currentJob()) });
        toast(t('ui.437'));
      }
      break;
    case 'retryFailed': {
      const j = currentJob(),
        overwrite = j?.plan?.options?.collisionPolicy === 'overwrite';
      if (
        overwrite &&
        (await confirmDialog(t('ui.438'), t('ui.439'), [
          { label: t('ui.329'), value: 'cancel' },
          { label: t('ui.440'), value: 'yes' },
        ])) !== 'yes'
      )
        break;
      const r = await request('retryFailed', { jobId: jobId(j), explicitOverwrite: overwrite });
      view.jobId = r?.attemptId || r?.jobId || '';
      go('queue');
      break;
    }
    case 'viewJob':
      view.jobId = el.dataset.value;
      go('complete');
      break;
    case 'showErrorItem':
      go('errors');
      break;
    case 'openOutput': {
      const j = currentJob(),
        p =
          j?.outputRoot ||
          j?.options?.outputRoot ||
          j?.plan?.options?.outputRoot ||
          draft.outputRoot;
      await request('openPath', { path: p });
      break;
    }
    case 'saveReport': {
      const r = await request('saveReport', {
        jobId: jobId(currentJob()),
        title: t('native.saveReportTitle'),
        filterLabel: t('native.jsonReportFilter'),
      });
      if (r.saved) toast(t('ui.441'));
      break;
    }
    case 'openLog':
      await request('openPath', { path: el.dataset.value });
      break;
    case 'copyError':
      await request('copyText', { text: el.dataset.value });
      toast(t('ui.442'));
      break;
    case 'saveSettings':
      await saveSettings();
      break;
    case 'settingTab':
      view.settingTab = el.dataset.value;
      if (view.settingTab === 'unity') go('unity');
      else {
        render();
        if (view.settingTab === 'storage') $('#setting-cache')?.scrollIntoView({ block: 'center' });
        if (view.settingTab === 'export') $('#setting-output')?.scrollIntoView({ block: 'center' });
      }
      break;
    case 'clearCache':
      if (
        (await confirmDialog(t('ui.443'), t('ui.444'), [
          { label: t('ui.329'), value: 'cancel' },
          { label: t('ui.445'), value: 'yes' },
        ])) === 'yes'
      ) {
        const r = await request('clearCache');
        toast(messageText(r, t('ui.446')));
      }
      break;
    case 'resetSettings':
      if (
        (await confirmDialog(t('ui.447'), t('ui.448'), [
          { label: t('ui.329'), value: 'cancel' },
          { label: t('ui.449'), value: 'yes' },
        ])) === 'yes'
      ) {
        settingsDraft = {
          ...native.settings,
          language: 'en',
          theme: 'dark',
          uiScale: 100,
          python: 'python',
          blender: '',
        };
        settingsDirty = true;
        render();
      }
      break;
    case 'dismissError':
      view.error = '';
      render();
      break;
  }
}
function sendResize(phase, event = resizeGesture) {
  if (!resizeGesture) return;
  request('window', {
    command: 'resize',
    phase,
    gesture: resizeGesture.id,
    direction: resizeGesture.direction,
    x: event.screenX,
    y: event.screenY,
  }).catch(showError);
}
function finishResize(phase, event = resizeGesture) {
  if (!resizeGesture) return;
  sendResize(phase, event);
  const { handle, pointerId } = resizeGesture;
  resizeGesture = null;
  if (handle.hasPointerCapture(pointerId)) handle.releasePointerCapture(pointerId);
}
document.addEventListener(
  'pointerdown',
  (event) => {
    const handle = event.target.closest('[data-window-resize]');
    if (!handle || event.button !== 0 || windowChrome.maximized) return;
    event.preventDefault();
    event.stopPropagation();
    resizeGesture = {
      id: ++resizeSerial,
      handle,
      pointerId: event.pointerId,
      direction: handle.dataset.windowResize,
      screenX: event.screenX,
      screenY: event.screenY,
    };
    handle.setPointerCapture(event.pointerId);
    sendResize('begin', event);
  },
  { capture: true },
);
document.addEventListener(
  'pointermove',
  (event) => {
    if (!resizeGesture || event.pointerId !== resizeGesture.pointerId) return;
    resizeGesture.screenX = event.screenX;
    resizeGesture.screenY = event.screenY;
    sendResize('update', event);
  },
  { capture: true },
);
document.addEventListener(
  'pointerup',
  (event) => {
    if (resizeGesture && event.pointerId === resizeGesture.pointerId) finishResize('end', event);
  },
  { capture: true },
);
document.addEventListener('pointercancel', () => finishResize('end'), { capture: true });
document.addEventListener('lostpointercapture', () => finishResize('end'), { capture: true });
document.addEventListener(
  'keydown',
  (event) => {
    if (!resizeGesture || event.key !== 'Escape') return;
    event.preventDefault();
    event.stopPropagation();
    finishResize('cancel');
  },
  { capture: true },
);

app.addEventListener('click', (e) => {
  const external = e.target.closest('[data-external-link]');
  if (external) {
    openExternalLink(external.dataset.externalLink);
    return;
  }
  const chrome = e.target.closest('[data-window-command]');
  if (chrome) {
    request('window', { command: chrome.dataset.windowCommand }).catch(showError);
    return;
  }
  const btn = e.target.closest('[data-action]');
  if (btn) {
    e.stopPropagation();
    Promise.resolve(action(btn.dataset.action, btn)).catch(showError);
    return;
  }
  const nav = e.target.closest('[data-page]');
  if (nav) {
    go(nav.dataset.page);
    return;
  }
  const item = e.target.closest('[data-clip]');
  if (item) {
    view.active = item.dataset.clip;
    saveView();
    // Preserve the clicked element long enough for the browser's double click.
    clearTimeout(cardClickTimer);
    cardClickTimer = setTimeout(render, 250);
  }
});
function previewTarget(element) {
  const target = element?.closest?.('canvas[data-viewer], [data-clip]');
  return target?.dataset.viewer || target?.dataset.clip || '';
}
app.addEventListener('pointerover', (e) => {
  if (!marquee?.active) hoverPreview(previewTarget(e.target));
});
app.addEventListener('pointerout', (e) => {
  if (previewTarget(e.target) !== previewTarget(e.relatedTarget)) hoverPreview(previewTarget(e.relatedTarget));
});
app.addEventListener('focusin', (e) => hoverPreview(previewTarget(e.target)));
app.addEventListener('focusout', (e) => hoverPreview(previewTarget(e.relatedTarget)));
document.addEventListener('visibilitychange', () => {
  lastTick = 0;
  hoverTick = 0;
  if (document.hidden) {
    cancelAnimationFrame(raf);
    raf = 0;
  } else {
    // Chromium may drop canvas pixels while the window is hidden; repaint every pose.
    for (const v of viewers) v.repaint();
    ensureAnimation();
  }
});
// The host moves the window from reported screen coordinates, so late delivery still works.
const headerDragTarget = (target) =>
  document.documentElement.dataset.customFrame !== 'false' &&
  target.closest?.('.topbar') &&
  !target.closest('button, a, input, select, textarea, .header-links, .window-controls');
function sendMove(phase, gesture = moveGesture, event = gesture) {
  if (!gesture) return Promise.resolve();
  return request('window', {
    command: 'move',
    phase,
    gesture: gesture.id,
    x: event.screenX,
    y: event.screenY,
  }).catch(showError);
}
function finishMove(phase, event) {
  const gesture = moveGesture;
  if (!gesture) return;
  if (event) {
    gesture.screenX = event.screenX;
    gesture.screenY = event.screenY;
  }
  moveGesture = null;
  sendMove(phase, gesture);
  if (gesture.header.hasPointerCapture(gesture.pointerId)) gesture.header.releasePointerCapture(gesture.pointerId);
  // Flush after this task so the click/dblclick dispatch still sees the original header.
  setTimeout(() => {
    if (!renderDeferred || moveGesture) return;
    renderDeferred = false;
    render();
  }, 0);
}
document.addEventListener(
  'pointerdown',
  (event) => {
    if (event.button !== 0 || windowChrome.maximized || moveGesture || !headerDragTarget(event.target)) return;
    const header = event.target.closest('.topbar');
    moveGesture = {
      id: ++moveSerial,
      header,
      pointerId: event.pointerId,
      screenX: event.screenX,
      screenY: event.screenY,
    };
    header.setPointerCapture(event.pointerId);
    sendMove('begin');
  },
  { capture: true },
);
document.addEventListener(
  'pointermove',
  (event) => {
    if (!moveGesture || event.pointerId !== moveGesture.pointerId) return;
    moveGesture.screenX = event.screenX;
    moveGesture.screenY = event.screenY;
    sendMove('update');
  },
  { capture: true },
);
document.addEventListener(
  'pointerup',
  (event) => {
    if (moveGesture && event.pointerId === moveGesture.pointerId) finishMove('end', event);
  },
  { capture: true },
);
document.addEventListener('pointercancel', () => finishMove('end'), { capture: true });
document.addEventListener('lostpointercapture', () => finishMove('end'), { capture: true });
app.addEventListener('dblclick', (e) => {
  if (e.button === 0 && headerDragTarget(e.target)) request('window', { command: 'maximize' }).catch(showError);
});
app.addEventListener('dblclick', (e) => {
  const item = e.target.closest('[data-clip]');
  if (item && !e.target.closest('[data-action]')) {
    clearTimeout(cardClickTimer);
    view.active = item.dataset.clip;
    go('preview');
    loadPreview();
  }
});
app.addEventListener('input', (e) => {
  const el = e.target;
  if (el.id === 'clip-search') {
    view.search = el.value;
    render();
    saveView();
  } else if (el.id === 'min-duration' || el.id === 'max-duration') {
    view[el.id === 'min-duration' ? 'min' : 'max'] = el.value;
    if (view.min !== '' && view.max !== '' && +view.min > +view.max) view.error = t('ui.450');
    else view.error = '';
    saveView();
  } else if (el.id === 'seek') {
    play.time = +el.value;
    const p = previews.get(view.active);
    if (p)
      viewers.filter((v) => v.canvas.dataset.viewer === view.active).forEach((v) =>
        v.setFrame(Math.min(p.frames.length - 1, Math.floor(play.time * (p.fps || 30)))),
      );
    if ($('#play-time'))
      $('#play-time').textContent = `${clock(play.time)} / ${clock(p?.duration)}`;
  } else fieldChanged(el);
});
app.addEventListener('change', (e) => {
  const el = e.target;
  fieldChanged(el);
  if (el.dataset.mappingSource) {
    draft.boneMap ||= {};
    if (el.value) draft.boneMap[el.dataset.mappingSource] = el.value;
    else delete draft.boneMap[el.dataset.mappingSource];
    native.skeleton = { ...native.skeleton, state: 'missing', valid: false };
    invalidatePreviews();
    render();
  } else if (el.id === 'sort') {
    view.sort = el.value;
    render();
    saveView();
  } else if (el.id === 'min-duration' || el.id === 'max-duration') render();
  else if (el.id === 'speed') play.speed = +el.value;
  else if (el.id === 'show-floor') {
    play.floor = el.checked;
    viewers.forEach((v) => {
      v.floor = play.floor;
      v.draw();
    });
  } else if (el.id === 'show-bones') {
    play.bones = el.checked;
    viewers.forEach((v) => {
      v.skeleton = play.bones;
      v.draw();
    });
  } else if (el.id === 'package-model') {
    draft.skeleton = el.value;
    native.skeleton = { state: 'missing' };
    invalidatePreviews();
    render();
  }
});
document.addEventListener('keydown', (e) => {
  if (modal.open) return;
  const modifier = document.documentElement.dataset.platform === 'macos' ? e.metaKey : e.ctrlKey;
  if (modifier && e.key.toLowerCase() === 'o') {
    e.preventDefault();
    openFiles();
  } else if (modifier && e.key.toLowerCase() === 'f') {
    e.preventDefault();
    if (view.page !== 'library') go('library');
    $('#clip-search')?.focus();
  } else if (e.key === 'Escape') {
    if (view.drawer) {
      view.drawer = false;
      render();
    } else if (view.filters) {
      view.filters = false;
      render();
    }
  } else if (e.target.matches('input,textarea,select')) return;
  else if (e.key === ' ') {
    if (view.page === 'preview') {
      e.preventDefault();
      action('play', e.target);
    } else {
      const item = e.target.closest('[data-clip]');
      if (item) {
        e.preventDefault();
        setSelected(item.dataset.clip);
      }
    }
  } else if (e.key === 'Enter') {
    const item = e.target.closest('[data-clip]');
    if (item) {
      view.active = item.dataset.clip;
      go('preview');
      loadPreview();
    }
  } else if (
    ['ArrowLeft', 'ArrowRight', 'ArrowUp', 'ArrowDown'].includes(e.key) &&
    e.target.closest('[data-clip]')
  ) {
    const items = [...document.querySelectorAll('[data-clip]')];
    const idx = items.indexOf(e.target.closest('[data-clip]'));
    const delta = e.key === 'ArrowLeft' || e.key === 'ArrowUp' ? -1 : 1;
    e.preventDefault();
    items[Math.max(0, Math.min(items.length - 1, idx + delta))]?.focus();
  }
});
// Back / forward from the keyboard and the mouse side buttons.
const TEXT_ENTRY_TYPES = new Set(['text', 'search', 'url', 'tel', 'email', 'password', 'number']);
function isTextEntry(el) {
  if (!el) return false;
  if (el.isContentEditable || el.tagName === 'TEXTAREA') return true;
  return el.tagName === 'INPUT' && TEXT_ENTRY_TYPES.has(el.type);
}
// WebView2 reports the mouse side buttons as button 3 (back, XButton1) and 4 (forward, XButton2);
// the host needs no extra handling.
const sideButtonDirection = { 3: 'back', 4: 'forward' };
document.addEventListener(
  'keydown',
  (e) => {
    if (e.defaultPrevented || e.repeat || e.ctrlKey || e.metaKey) return;
    let direction = '';
    if (e.altKey && !e.shiftKey) {
      if (e.key === 'ArrowLeft') direction = 'back';
      else if (e.key === 'ArrowRight') direction = 'forward';
    } else if (e.key === 'Backspace' && !e.altKey && !e.shiftKey && !isTextEntry(e.target)) {
      direction = 'back';
    }
    if (!direction) return;
    e.preventDefault();
    navigateHistory(direction);
  },
  { capture: true },
);
window.addEventListener(
  'mouseup',
  (e) => {
    const direction = sideButtonDirection[e.button];
    if (!direction) return;
    e.preventDefault();
    navigateHistory(direction);
  },
  { capture: true },
);
// Keep the browser from acting on the click it synthesizes for the side buttons.
window.addEventListener(
  'auxclick',
  (e) => {
    if (sideButtonDirection[e.button]) e.preventDefault();
  },
  { capture: true },
);
let dragDepth = 0;
document.addEventListener('dragenter', (e) => {
  if (e.dataTransfer?.types.includes('Files')) {
    e.preventDefault();
    dragDepth++;
    $('#drop-overlay').classList.add('visible');
  }
});
document.addEventListener('dragover', (e) => {
  if (e.dataTransfer?.types.includes('Files')) {
    e.preventDefault();
    e.dataTransfer.dropEffect = 'copy';
  }
});
document.addEventListener('dragleave', () => {
  dragDepth--;
  if (dragDepth <= 0) $('#drop-overlay').classList.remove('visible');
});
document.addEventListener('drop', (e) => {
  e.preventDefault();
  dragDepth = 0;
  $('#drop-overlay').classList.remove('visible');
  const files = Array.from(e.dataTransfer?.files || []);
  if (!files.length) return;
  if (hostTransport.additionalObjects) {
    recordPageChange('analysis');
    view.page = 'analysis';
    native.scan = { state: 'scanning', path: files.map((f) => f.name).join(', ') };
    render();
    request('drop', {}, files)
      .then((r) => {
        if (r?.accepted === false) throw new Error(t('ui.451'));
      })
      .catch((e) => {
        native.scan = { state: 'error' };
        showError(t('ui.452') + (e.message || e));
      });
  } else if (document.documentElement.dataset.platform === 'windows') toast(t('ui.453'));
});
window.addEventListener('beforeunload', saveView);
setLocale(native.settings.language);
localizeDocument();
render();
request('getAppInfo')
  .then((info) => {
    appInfo = info;
    render();
    // Automated UI tests start from a clean profile; keep the guide out of their way.
    if (!appInfo.provenance?.testHooks && !welcomeSeen()) showWelcome().then(() => setTimeout(checkForUpdate, 500));
    else setTimeout(checkForUpdate, 1500);
  })
  .catch(() => {});
async function checkForUpdate() {
  try {
    if (appInfo.provenance?.testHooks) return;
    const r = await request('checkForUpdate');
    if (!r?.supported || !r.available || !r.latest) return;
    let skipped = null;
    try { skipped = localStorage.getItem('animbridge.skippedUpdate'); } catch { /* storage unavailable */ }
    if (skipped === r.latest || modal.open) return;
    // Without the exact installer + checksum assets the release page is the fallback.
    const primary = r.installable
      ? { value: 'install', label: t('ui.500'), primary: true }
      : { value: 'open', label: t('ui.471'), primary: true };
    const answer = await confirmDialog(t('ui.469'), t('ui.470', { latest: r.latest, current: r.current || appInfo.version }), [
      primary,
      { value: 'skip', label: t('ui.472') },
      { value: 'later', label: t('ui.473') },
    ]);
    if (answer === 'install') {
      await startUpdate(r.latest);
    } else if (answer === 'open') {
      await openReleasePage();
    } else if (answer === 'skip') {
      try { localStorage.setItem('animbridge.skippedUpdate', r.latest); } catch { /* storage unavailable */ }
    }
  } catch {
    // Offline or rate limited: stay silent.
  }
}
async function openReleasePage() {
  try {
    await request('openExternalLink', { link: 'latestRelease' });
  } catch (e) {
    toast(e.message || String(e));
  }
}
// In-app update. The host downloads the installer, checks it against the release's
// SHA256SUMS.txt, starts it and closes the app; each step arrives as an
// "updateProgress" event with a `state` field. `updateDialog` is set while the
// progress dialog is open.
let updateDialog = null;
async function startUpdate(version) {
  updateDialog = { version, state: 'downloading' };
  showUpdateProgress({ state: 'downloading', received: 0, total: 0 });
  try {
    await request('installUpdate');
  } catch (e) {
    showUpdateProgress({ state: 'failed', message: e.message || String(e) });
  }
}
function megabytes(bytes) {
  return (Number(bytes || 0) / (1024 * 1024)).toFixed(1);
}
function updateProgressMarkup(data) {
  const total = Number(data.total || 0);
  const received = Number(data.received || 0);
  const percent = total ? Math.min(100, Math.round((received / total) * 100)) : null;
  const amount = total
    ? t('ui.503', { received: megabytes(received), total: megabytes(total) })
    : t('ui.504', { received: megabytes(received) });
  const bar = percent === null
    ? `<div class="progress-bar indeterminate"></div>`
    : `<div class="progress-bar" data-update-bar style="width:${percent}%"></div>`;
  return [
    `<div class="update-progress" role="progressbar" aria-valuemin="0" aria-valuemax="100"${percent === null ? '' : ` aria-valuenow="${percent}"`}>`,
    `<div class="progress-heading"><span class="small muted">${esc(amount)}</span>`,
    `<strong>${percent === null ? '' : `${percent}%`}</strong></div>`,
    `<div class="progress-track">${bar}</div></div>`,
  ].join('');
}
function showUpdateProgress(data) {
  if (!updateDialog) return; // events from an update this window did not start
  const state = data.state || 'downloading';
  if (state === 'cancelled') {
    updateDialog = null;
    if (modal.open) modal.close();
    toast(t('ui.510'));
    return;
  }
  // Downloading → only patch the bar so the Cancel button keeps focus.
  if (state === 'downloading' && updateDialog.state === 'downloading' && modal.open && modal.querySelector('.update-progress')) {
    modal.querySelector('.update-progress').outerHTML = updateProgressMarkup(data);
    return;
  }
  updateDialog.state = state;
  let title = t('ui.501');
  let text = t('ui.502', { version: updateDialog.version });
  let body = updateProgressMarkup(data);
  let buttons = `<button class="btn" data-update="cancel">${esc(t('ui.506'))}</button>`;
  if (state === 'verifying') {
    text = t('ui.505');
    body = updateProgressMarkup({});
  } else if (state === 'installing') {
    text = t('ui.507');
    body = updateProgressMarkup({});
    buttons = ''; // the app is closing; nothing left to cancel
  } else if (state === 'failed') {
    title = t('ui.508');
    text = `${messageText(data, t('ui.026'))} ${t('ui.509')}`;
    body = '';
    buttons = [
      `<button class="btn primary" data-update="open">${esc(t('ui.471'))}</button>`,
      `<button class="btn" data-update="close">${esc(t('ui.511'))}</button>`,
    ].join('');
  }
  if (modal.open) modal.close();
  modal.innerHTML = [
    `<h2 id="modal-title">${esc(title)}</h2><p>${esc(text)}</p>`,
    body ? `<div class="modal-content">${body}</div>` : '',
    `<div class="actions">${buttons}</div>`,
  ].join('');
  modal.onclick = (e) => {
    const action = e.target.closest('[data-update]')?.dataset.update;
    if (action === 'cancel') {
      e.target.disabled = true;
      request('cancelUpdate').catch(() => {});
    } else if (action === 'open' || action === 'close') {
      updateDialog = null;
      modal.close();
      if (action === 'open') openReleasePage();
    }
  };
  // Escape behaves like Cancel while downloading and is ignored while installing.
  modal.oncancel = (e) => {
    e.preventDefault();
    if (state === 'failed') {
      updateDialog = null;
      modal.close();
    } else if (state !== 'installing') {
      request('cancelUpdate').catch(() => {});
    }
  };
  modal.showModal();
  modal.querySelector('button')?.focus();
}
request('window', { command: 'getState' })
  .then(applyWindowState)
  .catch(() => {});
request('getState')
  .then(applyState)
  .catch((e) => {
    view.error = e.message;
    render();
  });
// SOURCE LIBRARY context menu. Right-click, the Menu key or Shift+F10 on a
// root opens it; Escape, Tab or a click elsewhere closes it.
let sourceMenu = null;
function closeSourceMenu(restoreFocus = true) {
  if (!sourceMenu) return;
  const { element, path } = sourceMenu;
  sourceMenu = null;
  element.remove();
  if (restoreFocus) app.querySelector(`[data-source-root="${CSS.escape(path)}"]`)?.focus();
}
function openSourceMenu(path, x, y) {
  closeSourceMenu(false);
  const element = document.createElement('div');
  element.className = 'context-menu';
  element.setAttribute('role', 'menu');
  element.innerHTML = [
    `<button role="menuitem" data-menu="reveal">${icon('folderOpen')}${esc(t('ui.541'))}</button>`,
    `<button role="menuitem" class="danger" data-menu="remove">${icon('trash')}${esc(t('ui.540'))}</button>`,
  ].join('');
  // The menu lives outside #app so a state render cannot wipe it.
  document.body.append(element);
  const box = element.getBoundingClientRect();
  element.style.left = `${Math.max(4, Math.min(x, innerWidth - box.width - 4))}px`;
  element.style.top = `${Math.max(4, Math.min(y, innerHeight - box.height - 4))}px`;
  sourceMenu = { element, path };
  element.querySelector('button').focus();
  element.addEventListener('keydown', (e) => {
    const items = [...element.querySelectorAll('[role="menuitem"]')];
    const index = items.indexOf(document.activeElement);
    if (e.key === 'Escape' || e.key === 'Tab') {
      e.preventDefault();
      e.stopPropagation();
      closeSourceMenu();
    } else if (e.key === 'ArrowDown' || e.key === 'ArrowUp') {
      e.preventDefault();
      const step = e.key === 'ArrowDown' ? 1 : -1;
      items[(index + step + items.length) % items.length].focus();
    } else if (e.key === 'Home' || e.key === 'End') {
      e.preventDefault();
      items[e.key === 'Home' ? 0 : items.length - 1].focus();
    }
  });
  element.addEventListener('click', (e) => {
    const item = e.target.closest('[data-menu]');
    if (!item) return;
    closeSourceMenu();
    if (item.dataset.menu === 'reveal') request('openPath', { path }).catch(showError);
    else removeLibrarySource(path).catch(showError);
  });
}
async function removeLibrarySource(path) {
  const root = sourceRoots().find((r) => samePath(r.path, path));
  if (!root) return;
  const answer = await confirmDialog(t('ui.542'), t('ui.543', { name: root.name, count: root.clips.length }), [
    { label: t('ui.329'), value: 'cancel' },
    { label: t('ui.540'), value: 'remove', primary: true },
  ]);
  if (answer !== 'remove') return;
  const ids = new Set(root.clips.map((c) => c.id));
  // The host drops the clips, the Recent row and the app-owned extraction;
  // the user's original files are never touched.
  await request('removeSource', { sourcePath: root.path });
  for (const id of ids) {
    selected.delete(id);
    previews.delete(id);
    metadata.delete(id);
  }
  if (ids.has(hoveredClip)) hoverPreview('');
  if (ids.has(view.active)) view.active = '';
  if (samePath(view.source, root.path)) {
    view.source = '';
    view.folder = '';
  }
  view.plan = null;
  saveView();
  render();
  toast(t('ui.544', { name: root.name }));
}
app.addEventListener('contextmenu', (e) => {
  const root = e.target.closest('[data-source-root]');
  if (!root) return;
  e.preventDefault();
  openSourceMenu(root.dataset.sourceRoot, e.clientX, e.clientY);
});
document.addEventListener(
  'keydown',
  (e) => {
    const root = e.target.closest?.('[data-source-root]');
    if (!root || !(e.key === 'ContextMenu' || (e.shiftKey && e.key === 'F10'))) return;
    e.preventDefault();
    e.stopPropagation();
    const box = root.getBoundingClientRect();
    openSourceMenu(root.dataset.sourceRoot, box.left + 24, box.bottom);
  },
  { capture: true },
);
document.addEventListener(
  'pointerdown',
  (e) => {
    if (sourceMenu && !sourceMenu.element.contains(e.target)) closeSourceMenu(false);
  },
  { capture: true },
);
window.addEventListener('blur', () => closeSourceMenu(false));
window.addEventListener('resize', () => closeSourceMenu(false));

// Marquee selection in the library: drag a rectangle over clip cards (grid)
// or rows (list) to select every one it touches. Ctrl/Cmd or Shift keeps the
// previous selection. A press that moves less than MARQUEE_THRESHOLD stays a
// normal click, so card clicks, checkboxes and double clicks work as before.
const MARQUEE_THRESHOLD = 5;
const MARQUEE_EDGE = 40; // Distance from the page edge that starts auto-scroll.
let marquee = null, suppressMarqueeClick = false;
function marqueePage(target) {
  if (view.page !== 'library' || modal.open) return null;
  const page = target.closest?.('.page');
  if (!page || !app.contains(page)) return null;
  // Controls and the toolbars above the clips keep their own behavior.
  const controls = 'button, a, input, select, textarea, label, summary, [data-action], ' +
    '.toolbar, .filter-panel, .folder-grid, .subtoolbar, .selection-footer, .page-heading';
  return target.closest(controls) ? null : page;
}
// Page-content coordinates stay correct while the page scrolls under the pointer.
function contentY(page, clientY) {
  return clientY - page.getBoundingClientRect().top + page.scrollTop;
}
function updateMarquee() {
  const page = app.querySelector('.page');
  if (!marquee || !page) return;
  const frame = page.getBoundingClientRect();
  const top = Math.min(marquee.startY, contentY(page, marquee.y));
  const bottom = Math.max(marquee.startY, contentY(page, marquee.y));
  const left = Math.min(marquee.startX, marquee.x);
  const right = Math.max(marquee.startX, marquee.x);
  // Draw only the part of the rectangle that is inside the scrolling page.
  const drawTop = Math.max(frame.top, top - page.scrollTop + frame.top);
  const drawBottom = Math.min(frame.bottom, bottom - page.scrollTop + frame.top);
  Object.assign(marquee.box.style, {
    left: `${left}px`,
    top: `${drawTop}px`,
    width: `${right - left}px`,
    height: `${Math.max(0, drawBottom - drawTop)}px`,
  });
  const next = new Set(marquee.additive ? marquee.base : []);
  for (const item of page.querySelectorAll('[data-clip]')) {
    const box = item.getBoundingClientRect();
    const itemTop = box.top - frame.top + page.scrollTop;
    const touched = box.left < right && box.right > left && itemTop < bottom && itemTop + box.height > top;
    if (touched) next.add(item.dataset.clip);
  }
  selected = next;
  // Cheap in-place feedback; the full render happens when the drag ends.
  for (const item of page.querySelectorAll('[data-clip]')) {
    const on = selected.has(item.dataset.clip);
    item.classList.toggle('checked', on);
    const check = item.querySelector('.check');
    if (check && check.getAttribute('aria-checked') !== String(on)) {
      check.setAttribute('aria-checked', String(on));
      check.innerHTML = on ? icon('check') : '';
    }
  }
}
function autoScrollMarquee() {
  if (!marquee?.active) return;
  const page = app.querySelector('.page');
  if (page) {
    const frame = page.getBoundingClientRect();
    let step = 0;
    if (marquee.y < frame.top + MARQUEE_EDGE) step = -Math.ceil((frame.top + MARQUEE_EDGE - marquee.y) / 4);
    else if (marquee.y > frame.bottom - MARQUEE_EDGE) step = Math.ceil((marquee.y - frame.bottom + MARQUEE_EDGE) / 4);
    if (step) {
      page.scrollTop += step;
      updateMarquee();
    }
  }
  marquee.frame = requestAnimationFrame(autoScrollMarquee);
}
function finishMarquee(keep) {
  const gesture = marquee;
  marquee = null;
  if (!gesture?.active) return;
  cancelAnimationFrame(gesture.frame);
  gesture.box.remove();
  document.body.classList.remove('marquee-active');
  if (document.body.hasPointerCapture(gesture.pointerId)) document.body.releasePointerCapture(gesture.pointerId);
  if (!keep) selected = gesture.base;
  // The browser still sends a click for this press; it must not open a card.
  suppressMarqueeClick = true;
  setTimeout(() => (suppressMarqueeClick = false), 0);
  render();
  if (keep) syncSelection();
}
document.addEventListener('pointerdown', (e) => {
  if (e.button !== 0 || marquee) return;
  const page = marqueePage(e.target);
  if (!page) return;
  marquee = {
    pointerId: e.pointerId,
    startX: e.clientX,
    startY: contentY(page, e.clientY),
    x: e.clientX,
    y: e.clientY,
    additive: e.ctrlKey || e.metaKey || e.shiftKey,
    base: new Set(selected),
    active: false,
  };
});
document.addEventListener('pointermove', (e) => {
  if (!marquee || e.pointerId !== marquee.pointerId) return;
  marquee.x = e.clientX;
  marquee.y = e.clientY;
  if (!marquee.active) {
    const page = app.querySelector('.page');
    const moved = Math.hypot(e.clientX - marquee.startX, contentY(page, e.clientY) - marquee.startY);
    if (moved < MARQUEE_THRESHOLD) return;
    marquee.active = true;
    marquee.box = document.createElement('div');
    marquee.box.className = 'marquee-box';
    document.body.append(marquee.box);
    document.body.classList.add('marquee-active');
    document.body.setPointerCapture(e.pointerId);
    window.getSelection()?.removeAllRanges();
    hoverPreview('');
    marquee.frame = requestAnimationFrame(autoScrollMarquee);
  }
  updateMarquee();
});
document.addEventListener('pointerup', (e) => {
  if (marquee && e.pointerId === marquee.pointerId) finishMarquee(true);
});
document.addEventListener('pointercancel', () => finishMarquee(true));
document.addEventListener(
  'keydown',
  (e) => {
    if (e.key !== 'Escape' || !marquee?.active) return;
    e.preventDefault();
    e.stopPropagation();
    finishMarquee(false);
  },
  { capture: true },
);
document.addEventListener(
  'click',
  (e) => {
    if (!suppressMarqueeClick) return;
    suppressMarqueeClick = false;
    e.preventDefault();
    e.stopPropagation();
  },
  { capture: true },
);
// Read-only inspection surface for the opt-in local UI acceptance runner.
window.animbridgeUI = {
  getState: () => ({ native, view, selected: [...selected], previewCount: previews.size }),
  navigate: go,
  request,
};
