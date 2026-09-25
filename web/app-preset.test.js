import assert from 'node:assert/strict';

// Preset chips must never claim a position the camera did not confirm:
// highlight only after an accepted goto has had time to travel, drop it on
// failure, on refusal, on jog and on home, and keep Delete's target separate.
const response = (status, data = {}) => ({
  status, ok: status >= 200 && status < 300,
  headers: { get: () => 'application/json' },
  json: async () => data, text: async () => JSON.stringify(data),
});
const ok = data => response(200, data);
const flush = async condition => {
  for (let n = 0; n < 2000 && !condition(); n++) await new Promise(r => realTimeout(r, 0));
  assert.ok(condition(), 'expected state was not reached');
};

class Element {
  constructor(tag = 'div') {
    this.tag = tag; this.children = []; this.dataset = {}; this.attrs = {};
    this.classes = new Set(); this.value = this.textContent = ''; this.disabled = false;
    this.elements = { hostname: { value: '' } };
    this.classList = {
      add: x => this.classes.add(x), remove: x => this.classes.delete(x),
      contains: x => this.classes.has(x),
      toggle: (x, force) => { if (force === undefined ? !this.classes.has(x) : force) this.classes.add(x); else this.classes.delete(x); },
    };
  }
  replaceChildren(...kids) { this.children = kids; }
  append(...kids) { this.children.push(...kids); }
  addEventListener(type, fn) { (this.listeners ||= {})[type] = fn; }
  setAttribute(k, v) { this.attrs[k] = v; }
  removeAttribute(k) { delete this.attrs[k]; }
}
const elements = new Map();
const get = s => { if (!elements.has(s)) elements.set(s, new Element()); return elements.get(s); };
const chips = () => get('#preset-chips').children;
const chip = token => chips().find(b => b.dataset.preset === token);
globalThis.document = {
  querySelector: get,
  querySelectorAll: sel => (sel.includes('#preset-chips button') ? chips() : []),
  activeElement: null, createElement: tag => new Element(tag), hidden: false, addEventListener() {},
};
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { userAgent: '' } });
globalThis.location = { search: '', protocol: 'https:', host: 'camera.local' };
globalThis.setInterval = () => 0;
// Fake only the long settle delay; keep short polling real so the test is fast.
const realTimeout = globalThis.setTimeout;
const settles = [];
globalThis.setTimeout = (fn, ms, ...a) => (ms >= 5000 ? settles.push(fn) : realTimeout(fn, ms, ...a));
const settleAll = () => { while (settles.length) settles.shift()(); };
globalThis.FormData = class { constructor() { this.fields = {}; } *[Symbol.iterator]() {} get() {} };
globalThis.localStorage = { getItem: () => null, setItem() {}, removeItem() {} };
globalThis.window = { addEventListener() {}, dispatchEvent() {}, confirm: () => false };

let gotoReply = () => ok({ state: 'complete', response: { status: 0 } });
const calls = [];
globalThis.fetch = (path, options = {}) => {
  calls.push({ path, method: options.method || 'GET' });
  if (path === '/api/v1/session') return ok({ csrf: 'S1' });
  if (path === '/api/v1/status') return ok({ features: {}, ssh_password_sync: true });
  if (path === '/api/v1/streams') return ok({ streams: [] });
  if (path === '/api/v1/network/mdns') return ok({ address: 'cam' });
  if (path === '/api/v1/ssh/authorized-keys') return ok({ authorized_keys: '' });
  if (path === '/api/v1/ptz/presets' && (options.method || 'GET') === 'GET') return ok({ status: '/list' });
  if (path === '/list') return ok({ state: 'complete', response: { ptz_coordinate: [
    { coordinateID: 0, name: 'PORTAO' }, { coordinateID: 1, name: 'RUA BAIXO' }] } });
  if (path === '/api/v1/ptz/presets' && options.method === 'PUT') return ok({ status: '/goto' });
  if (path === '/goto') return gotoReply();
  return ok({});
};
await import('./app.js');
await flush(() => chips().length === 2);
const gotos = () => calls.filter(c => c.path === '/api/v1/ptz/presets' && c.method === 'PUT').length;
const state = t => ({ lit: chip(t).classes.has('primary'), moving: chip(t).classes.has('pending') });

// Nothing is highlighted before the camera has been asked to go anywhere.
assert.deepEqual(state('0'), { lit: false, moving: false });

// An accepted goto shows "moving", becomes lit only after the travel window.
chip('0').onclick();
assert.deepEqual(state('0'), { lit: false, moving: true }, 'click must not claim arrival');
await flush(() => settles.length === 1);
assert.deepEqual(state('0'), { lit: false, moving: true });
settleAll();
await flush(() => state('0').lit);
assert.equal(state('0').moving, false);

// A second press while travelling is ignored (serialized motion).
chip('1').onclick();
const during = gotos();
chip('0').onclick();
assert.equal(gotos(), during, 'no second goto while one is travelling');
assert.equal(state('0').lit, false, 'old preset no longer lit once another goto starts');
await flush(() => settles.length === 1);
settleAll();
await flush(() => state('1').lit);

// A camera refusal clears the highlight instead of keeping the last click.
gotoReply = () => ok({ state: 'complete', response: { status: -1 } });
chip('0').onclick();
await flush(() => !state('0').moving);
assert.deepEqual(state('0'), { lit: false, moving: false });
assert.equal(state('1').lit, false, 'no stale highlight after a failed goto');

// A transport failure clears it too.
gotoReply = () => response(503, { error: { message: 'down' } });
chip('1').onclick();
await flush(() => !state('1').moving);
assert.deepEqual(state('1'), { lit: false, moving: false });

// Delete's target is the explicit choice and is styled separately.
assert.equal(chip('1').classes.has('chosen'), true);
assert.equal(chip('1').classes.has('primary'), false);
console.log('preset chips: no last-click highlight, serialized, cleared on failure: PASS');
