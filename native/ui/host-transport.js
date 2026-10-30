// This is the only module aware of WebView engines. Capabilities stay in this
// closure, are replaced on document bootstrap, and are never stored on disk.
let session = null;
let readyResolve;
let readyReject;
const ready = new Promise((resolve, reject) => { readyResolve = resolve; readyReject = reject; });
const listeners = new Set();
const grants = new Map();
let closed = false;
const pathPurposes = {
  source: 'read', path: 'read', model: 'read', skeleton: 'read',
  output: 'exportRoot', outputRoot: 'exportRoot',
  unityEditor: 'editor', unity_editor: 'editor', unreal_editor: 'editor', editorPath: 'editor', targetEditor: 'editor', python: 'editor', blender: 'editor',
  unityProject: 'engineProject', unity_project: 'engineProject', projectPath: 'engineProject', targetProject: 'engineProject',
};
function remember(rows) {
  for (const row of rows || []) if (row?.grant && row.path && row.purpose)
    grants.set(`${row.purpose}:${row.path}`, row.grant);
}
function prepare(value) {
  if (!value || typeof value !== 'object' || Array.isArray(value)) return value;
  const result = { ...value };
  for (const nested of ['options', 'settings']) if (result[nested]) result[nested] = prepare(result[nested]);
  for (const [field, purpose] of Object.entries(pathPurposes)) if (typeof result[field] === 'string' && result[field]) {
    const token = grants.get(`${purpose}:${result[field]}`);
    if (token) result[`${field}Grant`] = token;
  }
  if (Array.isArray(result.paths)) {
    result.grants = result.paths.map((path) => grants.get(`read:${path}`)).filter(Boolean);
    // Display strings cannot grant filesystem authority.
    delete result.paths;
  }
  return result;
}
function receive(input) {
  if (closed || window.top !== window) return;
  let message = input;
  if (typeof message === 'string') { try { message = JSON.parse(message); } catch { return; } }
  if (!message || typeof message !== 'object') return;
  if (message.event === 'bootstrap') {
    if (message.protocolVersion !== 1 || typeof message.epoch !== 'string' || typeof message.capability !== 'string' || message.capability.length < 32) return;
    session = { epoch: message.epoch, capability: message.capability, protocolVersion: 1 };
    document.documentElement.dataset.platform = message.data?.platform || '';
    document.documentElement.dataset.customFrame = String(!!message.data?.customFrame);
    document.documentElement.dataset.testHooks = String(!!message.data?.testHooks);
    readyResolve(message.data || {});
    return;
  }
  if (!session || message.protocolVersion !== 1 || message.epoch !== session.epoch) return;
  remember(message.result?.grants);
  remember(message.result?.pathGrants);
  remember(message.data?.pathGrants);
  for (const listener of listeners) listener(message);
}
window.__animbridgeReceive = receive;
window.chrome?.webview?.addEventListener('message', (event) => receive(event.data));
export const hostTransport = {
  ready,
  subscribe(listener) { listeners.add(listener); return () => listeners.delete(listener); },
  get additionalObjects() { return !!window.chrome?.webview?.postMessageWithAdditionalObjects; },
  async send(request, files = null) {
    await ready;
    if (closed || !session) throw new Error('The native view has closed.');
    const message = { ...request, payload: prepare(request.payload || {}), ...session };
    if (window.chrome?.webview) {
      if (files && window.chrome.webview.postMessageWithAdditionalObjects) window.chrome.webview.postMessageWithAdditionalObjects(message, files);
      else window.chrome.webview.postMessage(message);
    } else if (window.webkit?.messageHandlers?.animbridge) window.webkit.messageHandlers.animbridge.postMessage(message);
    else throw new Error('The native transport is unavailable.');
  },
};
window.addEventListener('pagehide', () => { closed = true; session = null; grants.clear(); readyReject(new Error('The native view has closed.')); });
