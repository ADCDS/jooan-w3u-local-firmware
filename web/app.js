'use strict';

const $ = s => document.querySelector(s);
const all = s => Array.from(document.querySelectorAll(s));

let csrf = '', wifiId = '', firmwareId = '';

/* ---------- notice ---------- */
/* The camera and the session fail differently, and the old UI reported both
   as "sign in again". Keep them apart: a transport failure leaves the session
   alone and says the camera is not answering. */
function notice(message, kind) {
  $('#notice-text').textContent = message;
  $('#notice').classList.remove('hidden');
  $('#notice').dataset.kind = kind || 'info';
}
function clearNotice() { $('#notice').classList.add('hidden'); }
$('#notice-close').onclick = clearNotice;

/* Every request here crosses a busy little camera, and some of them wake a
   shell helper: a second or two of nothing is normal. Without a pressed and a
   working state the only feedback for a press is silence, which reads as a
   dead button -- so people press it again. Holding the control disabled also
   stops the second press from opening a second session or a second upload. */
async function busy(el, fn) {
  if (!el) return fn();
  el.disabled = true;
  el.dataset.busy = '';
  try { return await fn(); } finally {
    delete el.dataset.busy;
    el.disabled = false;
  }
}

/* ---------- api ---------- */
class ApiError extends Error {
  constructor(message, kind, code) { super(message); this.kind = kind; this.code = code; }
}

/* A 401 on one camera route is not proof the cookie expired. In particular,
   never let an old response sign out a newer login. */
let authEpoch = 0, sessionCheck = null;
async function verifySession(epoch) {
  if (epoch !== authEpoch || !csrf) return;
  if (sessionCheck?.epoch === epoch) return sessionCheck.promise;
  const promise = (async () => {
    try {
      const response = await fetch('/api/v1/session', {
        headers: { Accept: 'application/json' }, cache: 'no-store',
      });
      if (epoch !== authEpoch || !csrf) return;
      if (response.status === 401) requireLogin(epoch);
      else if (response.ok) {
        const session = await response.json();
        if (epoch === authEpoch && session.csrf) csrf = session.csrf;
      }
      /* Offline/other errors are inconclusive: keep the current session. */
    } catch (_) { /* verification unavailable; do not sign out */ }
  })();
  sessionCheck = { epoch, promise };
  try { await promise; } finally { if (sessionCheck?.promise === promise) sessionCheck = null; }
}

async function api(path, options = {}) {
  const method = (options.method || 'GET').toUpperCase();
  const isLogin = path === '/api/v1/session' && method === 'POST';
  const isSession = path === '/api/v1/session';
  if (method !== 'GET' && method !== 'HEAD' && !isLogin && !csrf)
    throw new ApiError('Sign in before changing camera settings.', 'auth');
  const epoch = authEpoch;
  options.headers = Object.assign({ Accept: 'application/json' }, options.headers || {});
  if (method !== 'GET' && method !== 'HEAD' && !isLogin) options.headers['X-CSRF-Token'] = csrf;
  let response;
  try {
    response = await fetch(path, options);
  } catch (_) {
    throw new ApiError('The camera is not responding. It may be rebooting or off the network.', 'offline');
  }
  const type = response.headers.get('content-type') || '';
  const data = response.status === 204 ? null
    : type.includes('json') ? await response.json() : await response.text();
  if (!isSession && epoch !== authEpoch) throw new ApiError('Session changed. Please try again.', 'auth');
  if (!response.ok) {
    if (response.status === 401 && !isLogin && !(isSession && method === 'GET')) await verifySession(epoch);
    throw new ApiError(data?.error?.message || data?.error || data || `HTTP ${response.status}`,
      response.status === 401 ? 'auth' : 'api', data?.error?.code);
  }
  return data;
}

function show(id) {
  for (const s of ['#login', '#setup', '#console']) $(s).classList.add('hidden');
  $(id).classList.remove('hidden');
}

let autoSignInPromise = null;
function requireLogin(epoch = authEpoch, force = false) {
  if (epoch !== authEpoch || (!csrf && !force)) return;
  ++authEpoch;
  csrf = '';
  show('#login');
  /* A session that expired behind your back should not ask for the saved
     password again. Once, and never from inside its own failure. */
  if (!autoSignInPromise && readSaved()?.password) void autoSignIn();
}
window.addEventListener('joan-auth-required', () => { void verifySession(authEpoch); });

/* ---------- zones ---------- */
function setZone(zone) {
  $('#console').dataset.zone = zone;
  for (const b of all('#rail button')) b.setAttribute('aria-current', String(b.dataset.zone === zone));
  for (const s of all('.zone')) s.classList.toggle('hidden', s.id !== 'zone-' + zone);
}
for (const b of all('#rail button')) {
  b.onclick = () => {
    setZone(b.dataset.zone);
    if (b.dataset.zone === 'network') loadNetworkQuality();
  };
}

/* ---------- confirm guard ---------- */
/* Destructive actions get a second press, with Cancel first. */
function guard(host, message, verb, run) {
  host.replaceChildren();
  const box = document.createElement('div');
  box.className = 'confirm';
  const text = document.createElement('p');
  text.textContent = message;
  const row = document.createElement('div');
  row.className = 'grid2';
  const cancel = document.createElement('button');
  cancel.textContent = 'Cancel';
  cancel.dataset.autofocus = '1';
  const go = document.createElement('button');
  go.textContent = verb;
  go.className = 'danger';
  const close = () => { host.replaceChildren(); };
  cancel.onclick = close;
  go.onclick = () => { close(); run(); };
  row.append(cancel, go);
  box.append(text, row);
  host.append(box);
}

/* ---------- status ---------- */
const FEATURE_LABELS = {
  https_identity: ['HTTPS identity', 'device key'],
  rtsp: ['RTSP', 'digest auth'],
  ptz: ['Pan / tilt', null],
  ssh: ['SSH', null],
  mdns: ['mDNS', null],
};

function chip(label, value, tone) {
  const el = document.createElement('span');
  el.className = 'pill' + (tone ? ' ' + tone : '');
  el.innerHTML = '<i class="led"></i>';
  el.append(label + ' ');
  const b = document.createElement('b');
  b.textContent = value;
  el.append(b);
  return el;
}

function renderStatus(s) {
  const build = keys => keys.map(key => {
    const raw = s.features?.[key];
    if (raw === undefined) return null;
    const [label, fixed] = FEATURE_LABELS[key];
    const value = raw === true ? (fixed || 'on') : raw === false ? 'off' : String(raw);
    return chip(label, value, raw === false ? 'off' : '');
  }).filter(Boolean);
  $('#state-chips').replaceChildren(...build(Object.keys(FEATURE_LABELS)));

  const rows = [
    ['Build', `${s.version || '—'} · ${s.state || '—'}`],
    ['Approved routes', (s.active_routes?.cidrs || []).join(', ') || 'none added'],
    ['MQTT bridge', s.mqtt_bridge || '—'],
  ];
  if (s.https_cloud_bridge && !s.https_cloud_bridge.supported) {
    rows.push(['Cloud bridge', s.https_cloud_bridge.blocker || 'unsupported']);
  }
  $('#state-detail').replaceChildren(...rows.map(([k, v]) => {
    const row = document.createElement('div');
    row.className = 'row';
    const key = document.createElement('span');
    key.textContent = k;
    const val = document.createElement('b');
    val.textContent = v;
    row.append(key, val);
    return row;
  }));

  /* Only the SSH mismatch is worth a banner: it means SSH will refuse the
     password you just used. That the initial password is still set is a fact
     the System zone already states, not something to interrupt every screen. */
  $('#ssh-warning').classList.toggle('hidden', !!s.ssh_password_sync);
  $('#pw-state').textContent = s.default_password_warning ? 'initial value' : 'changed';
}

/* ---------- load ---------- */
async function loadStatus() {
  const epoch = authEpoch;
  const s = await api('/api/v1/status');
  if (epoch !== authEpoch || !csrf) return null;
  renderStatus(s);
  show('#console');
  return s;
}

async function load() {
  if (!await loadStatus()) return;
  const [streamList, mdns] = await Promise.all([
    api('/api/v1/streams'), api('/api/v1/network/mdns'),
  ]);
  $('#rail-host').textContent = mdns.address || '';
  /* What an NVR needs: both sensors over RTSP, and ONVIF on this very port for
     pan/tilt and presets (sensor A). */
  $('#streams').replaceChildren(...[
    ...streamList.streams.map(s => [`${s.id} · ${s.width}×${s.height}`, s.rtsp]),
    ['ONVIF', `https://${location.host}/onvif/device_service`],
  ].map(([label, value]) => {
    const row = document.createElement('div');
    row.className = 'row';
    const k = document.createElement('span'); k.textContent = label;
    const v = document.createElement('b'); v.textContent = value;
    row.append(k, v); return row;
  }));

  $('#mdns-form').elements.hostname.value = mdns.hostname;
  $('#mdns-result').textContent = `https://${mdns.address}/`;
  try {
    const keys = await api('/api/v1/ssh/authorized-keys');
    const lines = (keys.authorized_keys || '').split('\n').filter(Boolean);
    $('#ssh-list').replaceChildren(...(lines.length ? lines : ['none']).map(line => {
      const row = document.createElement('div');
      row.className = 'row';
      const k = document.createElement('span');
      k.textContent = 'key';
      const v = document.createElement('b');
      v.textContent = line;
      row.append(k, v);
      return row;
    }));
  } catch (x) { $('#ssh-list').textContent = x.message; }
}

/* ---------- auth ---------- */
/* Staying signed in: the credential can be kept here and replayed on load.
 *
 * It is kept in clear text in this origin's localStorage, which is the honest
 * description and the reason it is opt-in and off by default: anyone who can
 * reach this browser profile can read it. It buys nothing against an attacker
 * who is already on the camera's network -- the camera is the thing being
 * protected -- and it is discarded the moment the credential stops working,
 * so a changed password does not leave a stale copy lying around. */
const SAVED = 'joan-credential';
const readSaved = () => {
  try { return JSON.parse(localStorage.getItem(SAVED) || 'null'); } catch (_) { return null; }
};
const forgetSaved = () => { try { localStorage.removeItem(SAVED); } catch (_) { /* private mode */ } };

async function signIn(credential, remember) {
  const epoch = ++authEpoch;
  csrf = '';
  try {
    const d = await api('/api/v1/session', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(credential),
    });
    if (epoch !== authEpoch) return false;
    if (!d.csrf) throw new ApiError('Sign in did not return a security token.', 'auth');
    csrf = d.csrf;
    if (remember) {
      try { localStorage.setItem(SAVED, JSON.stringify(credential)); } catch (_) { /* private mode */ }
    }
    clearNotice();
    await load();
    return true;
  } catch (x) {
    if (epoch !== authEpoch) return false; // an older login must not affect the new one
    throw x;
  }
}

$('#login-form').addEventListener('submit', e => {
  e.preventDefault();
  busy(e.submitter || $('#login-form button'), async () => {
    try {
      await signIn(Object.fromEntries(new FormData(e.target)), $('#remember').checked);
    } catch (x) { notice(x.message, 'crit'); }
  });
});

/* A saved credential that no longer works is worse than none: it would fail
   on every load and strand you behind a form you cannot see. Drop it and show
   the form instead. Coalesce boot and expired-session replay. */
function autoSignIn() {
  if (autoSignInPromise) return autoSignInPromise;
  const saved = readSaved();
  if (!saved?.password) return Promise.resolve(false);
  const run = (async () => {
    $('#remember').checked = true;
    const epoch = authEpoch + 1;
    try {
      return await busy($('#login-form button'), () => signIn(saved, false));
    } catch (x) {
      if (authEpoch !== epoch) return false;
      if (x.kind === 'auth' && !csrf) {
        forgetSaved();
        $('#remember').checked = false;
      }
      notice(x.kind === 'auth' && !csrf ? 'The saved password no longer works.' : x.message, 'warn');
      return false;
    }
  })();
  autoSignInPromise = run;
  void run.finally(() => { if (autoSignInPromise === run) autoSignInPromise = null; });
  return run;
}



$('#password-form').addEventListener('submit', e => {
  e.preventDefault();
  busy(e.submitter || $('#password-form button'), async () => {
    try {
      await api('/api/v1/setup/password', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify(Object.fromEntries(new FormData(e.target))),
      });
      ++authEpoch;
      csrf = '';
      forgetSaved(); // saved credentials contain the old password
      $('#remember').checked = false;
      show('#login');
      notice('Password changed. Sign in again.');
    } catch (x) { notice(x.message, 'crit'); }
  });
});

$('#change-password').onclick = () => show('#setup');

/* ---------- camera time ---------- */
/* The camera has no RTC and no NTP route, so its clock defaults years off,
   which shows in the burned-in OSD overlay and the recorded streams. Push
   this device's clock (true UTC) so it keeps correct time. Idle sessions use
   a monotonic server clock, unaffected by setting the wall clock. */
function tickLocalTime() { const el = $('#time-local'); if (el) el.textContent = new Date().toLocaleString(); }
setInterval(tickLocalTime, 1000); tickLocalTime();
async function setCameraTime(epochSeconds) {
  const d = await api('/api/v1/time', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ epoch: String(epochSeconds) }),
  });
  $('#time-camera').textContent = new Date(d.epoch * 1000).toLocaleString();
}
/* Seed the manual field with the current local time as a starting point. */
function seedManualTime() {
  const el = $('#time-manual');
  if (!el || el === document.activeElement) return;
  const n = new Date(Date.now() - new Date().getTimezoneOffset() * 60000);
  el.value = n.toISOString().slice(0, 19);
}
setInterval(seedManualTime, 1000); seedManualTime();
/* jooanipc burns the OSD clock from the camera's stored GMT offset, and the
   camera has no zoneinfo database. So this device computes the offset string the
   firmware expects: getTimezoneOffset() is minutes behind UTC, and the stored
   "GMT±HH:MM" carries the inverted sign the OEM applies (UTC+8 stores GMT+08:00). */
function browserTimeZone() {
  const off = new Date().getTimezoneOffset();
  const sign = off <= 0 ? '+' : '-';
  const abs = Math.abs(off);
  const hh = String(Math.floor(abs / 60)).padStart(2, '0');
  const mm = String(abs % 60).padStart(2, '0');
  return `GMT${sign}${hh}:${mm}`;
}
async function setCameraTimezone() {
  return api('/api/v1/timezone', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ gmt_tz: browserTimeZone() }),
  });
}
/* One time write at a time; reload only after all selected writes.
   A failed write must not trigger a reload that races the next PUT. */
let timeBusy = false;
async function writeCameraTime(epoch, zone) {
  if (timeBusy) return;
  timeBusy = true;
  $('#time-sync').disabled = $('#time-set-manual').disabled = true;
  try {
    await setCameraTime(epoch);
    if (zone) {
      const d = await setCameraTimezone();
      $('#time-result').textContent =
        `Clock synced; time zone ${d.gmt_tz} saved. The overlay updates after a restart.`;
    } else $('#time-result').textContent = 'Camera clock set.';
    await load();
  } catch (x) { notice(x.message, 'crit'); }
  finally {
    timeBusy = false;
    $('#time-sync').disabled = $('#time-set-manual').disabled = false;
  }
}
$('#time-sync').onclick = () => writeCameraTime(Math.floor(Date.now() / 1000), true);
$('#time-set-manual').onclick = () => {
  const v = $('#time-manual').value;
  if (!v) { notice('Choose a date and time first.'); return; }
  const epoch = Math.floor(new Date(v).getTime() / 1000);
  if (!Number.isFinite(epoch)) { notice('That date and time could not be read.'); return; }
  return writeCameraTime(epoch, false);
};
$('#goto-password').onclick = () => show('#setup');
$('#setup-cancel').onclick = () => show('#console');
$('#refresh').onclick = () => load().catch(x => notice(x.message, 'crit'));
$('#logout').onclick = () => busy($('#logout'), async () => {
  /* Forget before requireLogin: signing out must never replay saved credentials. */
  forgetSaved();
  const epoch = ++authEpoch; // fence in-flight checks and logins before sign-out
  try { await api('/api/v1/session', { method: 'DELETE' }); } catch (_) { /* ending anyway */ }
  requireLogin(epoch, true);
});

/* ---------- network ---------- */
/* Never interpolate network/SSID values as HTML. No credential or address is
   supplied by the quality endpoint, and all unavailable fields stay visible. */
function networkQualityField(section, label, value) {
  const row = document.createElement('div');
  row.className = 'kv-row';
  const key = document.createElement('span'); key.textContent = label;
  const display = document.createElement('b'); display.textContent = value ?? 'Unknown';
  row.append(key, display);
  section.append(row);
}
async function loadNetworkQuality() {
  const report = $('#network-quality-report');
  report.textContent = 'Checking network quality…';
  try {
    const data = await api('/api/v1/network/quality');
    report.replaceChildren();
    for (const [title, item, stateLabel, state] of [
      ['Wi-Fi', data.wifi || {}, 'Association', data.wifi?.associated === true ? 'Connected' : data.wifi?.associated === false ? 'Disconnected' : null],
      ['Ethernet', data.ethernet || {}, 'Link', data.ethernet?.link === true ? 'Up' : data.ethernet?.link === false ? 'Down' : null],
    ]) {
      const section = document.createElement('div');
      const heading = document.createElement('h3'); heading.textContent = title;
      section.append(heading);
      networkQualityField(section, stateLabel, state);
      if (title === 'Wi-Fi') {
        networkQualityField(section, 'SSID', item.ssid);
        networkQualityField(section, 'Signal', item.signal_dbm == null ? null : `${item.signal_dbm} dBm`);
        networkQualityField(section, 'Rate', item.rate_mbps == null ? null : `${item.rate_mbps} Mb/s`);
      } else networkQualityField(section, 'Speed', item.speed_mbps == null ? null : `${item.speed_mbps} Mb/s`);
      for (const [key, label] of [['rx_bytes', 'Received bytes'], ['tx_bytes', 'Sent bytes'],
        ['rx_packets', 'Received packets'], ['tx_packets', 'Sent packets']]) {
        networkQualityField(section, label, item[key]);
      }
      report.append(section);
    }
  } catch (error) {
    report.textContent = `Network quality unavailable: ${error.message}`;
  }
}
$('#network-quality-refresh').onclick = () => busy($('#network-quality-refresh'), loadNetworkQuality);

$('#wifi-form').addEventListener('submit', async e => {
  e.preventDefault();
  try {
    const d = await api('/api/v1/network/wifi', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(Object.fromEntries(new FormData(e.target))),
    });
    wifiId = d.id || '';
    $('#wifi-result').textContent = 'Staged. Commit once the camera has reconnected.';
  } catch (x) { notice(x.message, 'crit'); }
});
$('#wifi-commit').onclick = () => api('/api/v1/network/wifi', {
  method: 'PUT', headers: { 'Content-Type': 'application/json' },
  body: JSON.stringify({ id: wifiId }),
}).then(() => { $('#wifi-result').textContent = 'Committed.'; })
  .catch(x => notice(x.message, 'crit'));
$('#wifi-rollback').onclick = () => guard($('#wifi-confirm'),
  'Roll back to the OEM Wi-Fi setting? This drops the current link.', 'Roll back',
  () => api('/api/v1/network/wifi', {
    method: 'DELETE', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ id: wifiId }),
  }).then(() => { $('#wifi-result').textContent = 'Rolled back.'; })
    .catch(x => notice(x.message, 'crit')));

$('#mdns-form').addEventListener('submit', async e => {
  e.preventDefault();
  try {
    const d = await api('/api/v1/network/mdns', {
      method: 'PUT', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(Object.fromEntries(new FormData(e.target))),
    });
    $('#mdns-result').textContent = `https://${d.address}/`;
    $('#rail-host').textContent = d.address;
  } catch (x) { notice(x.message, 'crit'); }
});

$('#routes-form').addEventListener('submit', async e => {
  e.preventDefault();
  try {
    const d = await api('/api/v1/network/routes', {
      method: 'PUT', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(Object.fromEntries(new FormData(e.target))),
    });
    $('#routes-result').textContent = (d.cidrs || []).join(', ') || 'none';
  } catch (x) { notice(x.message, 'crit'); }
});

$('#ssh-form').addEventListener('submit', async e => {
  e.preventDefault();
  try {
    await api('/api/v1/ssh/authorized-keys', {
      method: 'POST', headers: { 'Content-Type': 'text/plain' },
      body: new FormData(e.target).get('key'),
    });
    e.target.reset();
    await load();
  } catch (x) { notice(x.message, 'crit'); }
});

/* ---------- firmware ---------- */
$('#firmware-form').addEventListener('submit', async e => {
  e.preventDefault();
  try {
    const d = await api('/api/v1/update', {
      method: 'POST', headers: { 'Content-Type': 'application/octet-stream' },
      body: new FormData(e.target).get('image'),
    });
    firmwareId = d.id || '';
    $('#firmware-result').textContent = `Staged ${d.version || d.id || ''} · signature verified`;
  } catch (x) { notice(x.message, 'crit'); }
});
$('#firmware-apply').onclick = () => {
  if (!firmwareId) { notice('Stage a verified image first.'); return; }
  guard($('#firmware-confirm'),
    'Apply the staged image and reboot? The camera is offline for about 90 seconds.', 'Apply', () =>
    api('/api/v1/update', {
      method: 'PUT', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ id: firmwareId }),
    }).then(() => { $('#firmware-result').textContent = 'Applying. The camera is rebooting.'; })
      .catch(x => notice(x.message, 'crit')));
};

/* ---------- boot ---------- */
async function resume() {
  const epoch = authEpoch;
  const session = await api('/api/v1/session');
  if (epoch !== authEpoch) return;
  if (!session.csrf) throw new ApiError('Session did not return a security token.', 'auth');
  csrf = session.csrf;
  await load();
}
setZone('system');
const bootEpoch = authEpoch;
resume().catch(x => {
  if (authEpoch !== bootEpoch || csrf) return; // another sign-in won the race
  if (x.kind === 'offline') { notice(x.message, 'crit'); show('#login'); return; }
  /* Show the form first: a slow camera should not leave a blank screen while
     the saved credential is replayed. */
  show('#login');
  void autoSignIn();
});
/* Retire any service worker installed by an earlier build; it cached index.html
   and app.js and would keep serving the pre-update UI after a firmware upgrade. */
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.getRegistrations()
    .then(list => list.forEach(reg => reg.unregister())).catch(() => {});
}
if (window.caches) caches.keys().then(keys => keys.forEach(k => caches.delete(k))).catch(() => {});
