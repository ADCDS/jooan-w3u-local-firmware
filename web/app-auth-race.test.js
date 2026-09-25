import assert from 'node:assert/strict';

const gate = () => {
  let resolve;
  return { promise: new Promise(ok => { resolve = ok; }), resolve };
};
const response = (status, data = {}) => ({
  status, ok: status >= 200 && status < 300,
  headers: { get: () => 'application/json' },
  json: async () => data, text: async () => JSON.stringify(data),
});
const ok = data => response(200, data);
const unauth = () => response(401, { error: { message: 'Sign in again' } });
const flush = async condition => {
  for (let n = 0; n < 1000 && !condition(); n++) await Promise.resolve();
  assert.ok(condition(), 'expected asynchronous request did not arrive');
};

class Element {
  constructor(hidden = false) {
    this.classes = new Set(hidden ? ['hidden'] : []);
    this.classList = {
      add: x => this.classes.add(x), remove: x => this.classes.delete(x),
      contains: x => this.classes.has(x),
      toggle: (x, force) => { if (force === undefined ? !this.classes.has(x) : force) this.classes.add(x); else this.classes.delete(x); },
    };
    this.dataset = {};
    this.elements = { hostname: { value: '' } };
    this.value = this.textContent = '';
    this.disabled = false;
  }
  replaceChildren() {}
  append() {}
  addEventListener(type, fn) { (this.listeners ||= {})[type] = fn; }
  setAttribute() {}
  removeAttribute() {}
}
const elements = new Map();
const get = selector => {
  if (!elements.has(selector)) elements.set(selector, new Element(selector === '#console' || selector === '#setup'));
  return elements.get(selector);
};
globalThis.document = {
  querySelector: get, querySelectorAll: () => [], activeElement: null,
  createElement: () => new Element(), hidden: false, addEventListener() {},
};
Object.defineProperty(globalThis, 'navigator', { configurable: true, value: { userAgent: '' } });
globalThis.location = { search: '', protocol: 'https:', host: 'camera.local' };
globalThis.setInterval = () => 0;
globalThis.FormData = class {
  constructor(form) { this.fields = form.fields || {}; }
  *[Symbol.iterator]() { yield* Object.entries(this.fields); }
  get(key) { return this.fields[key]; }
};
const saved = new Map();
globalThis.localStorage = {
  getItem: key => saved.get(key) ?? null,
  setItem: (key, value) => saved.set(key, value),
  removeItem: key => saved.delete(key),
};
const listeners = new Map();
globalThis.window = {
  addEventListener: (type, fn) => listeners.set(type, fn),
  dispatchEvent: event => listeners.get(event.type)?.(event),
};
const calls = [];
let intercept = null;
globalThis.fetch = (path, options = {}) => {
  calls.push({ path, options });
  const override = intercept?.(path, options);
  if (override) return override;
  if (path === '/api/v1/session') {
    if (options.method === 'POST') return ok({ csrf: 'S2' });
    if (options.method === 'DELETE') return response(204);
    return ok({ csrf: 'S1' });
  }
  if (path === '/api/v1/status') return ok({ features: {}, ssh_password_sync: true });
  if (path === '/api/v1/streams') return ok({ streams: [] });
  if (path === '/api/v1/network/mdns') return ok({ address: 'camera.local', hostname: 'camera' });
  if (path === '/api/v1/ssh/authorized-keys') return ok({ authorized_keys: '' });
  if (path === '/api/v1/ptz/presets') return ok({ status: '/op' });
  if (path === '/op') return ok({ state: 'complete', response: { presets: [] } });
  if (path === '/api/v1/time') return ok({ epoch: 1730000000 });
  if (path === '/api/v1/timezone') return ok({ gmt_tz: 'GMT+00:00' });
  return ok({});
};
await import('./app.js');
const click = id => get(id).onclick();
const submit = (id, fields) => {
  const form = get(id);
  form.fields = fields;
  return form.listeners.submit({ preventDefault() {}, target: form, submitter: get(`${id} button`) });
};
const count = (path, method) => calls.filter(x => x.path === path && (!method || (x.options.method || 'GET') === method)).length;
const last = (path, method) => calls.findLast(x => x.path === path && (!method || (x.options.method || 'GET') === method));
const visible = id => !get(id).classList.contains('hidden');
await flush(() => count('/api/v1/ssh/authorized-keys') === 1);
assert.equal(visible('#console'), true);

// A failed mutation is not retried. Multiple route/video 401s share one session
// check; an authenticated cookie leaves the console and saved login untouched.
{
  saved.set('joan-credential', JSON.stringify({ username: 'admin', password: 'keep' }));
  const check = gate();
  let checks = 0;
  intercept = (path, options) => {
    if (path === '/api/v1/time') return unauth();
    if (path === '/api/v1/status') return unauth();
    if (path === '/api/v1/session' && !options.method) { checks++; return check.promise; }
  };
  const mutation = click('#time-sync');
  const failedRead = click('#refresh');
  window.dispatchEvent({ type: 'joan-auth-required' });
  await flush(() => checks === 1);
  assert.equal(count('/api/v1/time', 'PUT'), 1);
  assert.equal(last('/api/v1/time', 'PUT').options.headers['X-CSRF-Token'], 'S1');
  check.resolve(ok({ csrf: 'S1' }));
  await Promise.all([mutation, failedRead]);
  assert.equal(checks, 1);
  assert.equal(count('/api/v1/time', 'PUT'), 1);
  assert.equal(count('/api/v1/timezone', 'PUT'), 0, 'failed clock write cannot continue to zone write');
  assert.equal(visible('#console'), true);
  assert.equal(count('/api/v1/session', 'POST'), 0, 'valid session must not replay saved password');
  assert.equal(saved.has('joan-credential'), true);
  intercept = null;
}

// Successful clock synchronization waits for timezone PUT, then reloads once.
// Disabled controls and a synchronous in-progress guard block duplicate writes.
{
  const zone = gate();
  const statuses = count('/api/v1/status');
  const times = count('/api/v1/time', 'PUT');
  intercept = (path, options) => path === '/api/v1/timezone' && options.method === 'PUT' ? zone.promise : null;
  const sync = click('#time-sync');
  await flush(() => count('/api/v1/timezone', 'PUT') === 1);
  assert.equal(get('#time-sync').disabled, true);
  assert.equal(get('#time-set-manual').disabled, true);
  get('#time-manual').value = '2025-01-01T12:00:00';
  await click('#time-set-manual');
  assert.equal(count('/api/v1/time', 'PUT'), times + 1);
  assert.equal(count('/api/v1/status'), statuses, 'reload must not start before zone finishes');
  assert.equal(last('/api/v1/timezone', 'PUT').options.headers['X-CSRF-Token'], 'S1');
  zone.resolve(ok({ gmt_tz: 'GMT+00:00' }));
  await sync;
  assert.equal(count('/api/v1/status'), statuses + 1);
  assert.equal(get('#time-sync').disabled, false);
  intercept = null;
}

// Session expiry verified by GET 401 signs out once. Saved credentials replay
// once; a mutation with empty csrf never reaches fetch while login is pending.
{
  const login = gate();
  const statuses = count('/api/v1/status');
  const times = count('/api/v1/time', 'PUT');
  intercept = (path, options) => {
    if (path === '/api/v1/session' && !options.method) return unauth();
    if (path === '/api/v1/session' && options.method === 'POST') return login.promise;
  };
  window.dispatchEvent({ type: 'joan-auth-required' });
  await flush(() => count('/api/v1/session', 'POST') === 1);
  assert.equal(visible('#login'), true);
  get('#time-manual').value = '2025-01-01T12:00:00';
  await click('#time-set-manual');
  assert.equal(count('/api/v1/time', 'PUT'), times);
  window.dispatchEvent({ type: 'joan-auth-required' });
  assert.equal(count('/api/v1/session', 'POST'), 1);
  login.resolve(ok({ csrf: 'S2' }));
  await flush(() => count('/api/v1/status') > statuses);
  await flush(() => visible('#console'));
  intercept = null;
}

// A verification response from the old auth epoch cannot clear a newer login.
{
  const oldCheck = gate();
  let checks = 0;
  intercept = (path, options) => {
    if (path === '/api/v1/session' && !options.method) { checks++; return oldCheck.promise; }
    if (path === '/api/v1/session' && options.method === 'POST') return ok({ csrf: 'S3' });
  };
  window.dispatchEvent({ type: 'joan-auth-required' });
  await flush(() => checks === 1);
  await submit('#login-form', { username: 'admin', password: 'new' });
  await flush(() => visible('#console'));
  oldCheck.resolve(unauth());
  await Promise.resolve(); await Promise.resolve();
  assert.equal(visible('#console'), true);
  intercept = null;
  const times = count('/api/v1/time', 'PUT');
  await click('#time-set-manual');
  assert.equal(count('/api/v1/time', 'PUT'), times + 1);
  assert.equal(last('/api/v1/time', 'PUT').options.headers['X-CSRF-Token'], 'S3');
}

// A late 401 from an older login cannot clear a successful newer session.
{
  const old = gate();
  let logins = 0;
  intercept = (path, options) => {
    if (path === '/api/v1/session' && options.method === 'POST') return ++logins === 1 ? old.promise : ok({ csrf: 'S4' });
  };
  const statuses = count('/api/v1/status');
  submit('#login-form', { username: 'admin', password: 'old' });
  await flush(() => logins === 1);
  submit('#login-form', { username: 'admin', password: 'new' });
  await flush(() => logins === 2 && count('/api/v1/status') > statuses);
  old.resolve(unauth());
  await flush(() => count('/api/v1/ssh/authorized-keys') > 1);
  assert.equal(visible('#console'), true);
  assert.equal(saved.has('joan-credential'), true);
  intercept = null;
  await click('#time-set-manual');
  assert.equal(last('/api/v1/time', 'PUT').options.headers['X-CSRF-Token'], 'S4');
}

// Explicit sign-out forgets stored credentials, closes UI, and guards writes.
{
  const times = count('/api/v1/time', 'PUT');
  const logins = count('/api/v1/session', 'POST');
  await click('#logout');
  assert.equal(visible('#login'), true);
  assert.equal(saved.has('joan-credential'), false);
  assert.equal(count('/api/v1/session', 'POST'), logins);
  await click('#time-set-manual');
  assert.equal(count('/api/v1/time', 'PUT'), times);
}
console.log('web app auth epochs, 401 verification, CSRF guards, and clock write races: PASS');
