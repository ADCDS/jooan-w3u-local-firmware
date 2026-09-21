'use strict';
import { JooanAudioClient } from './audio-client.js';
import { Fmp4Player } from './video-player.js';

const $ = s => document.querySelector(s);
const all = s => Array.from(document.querySelectorAll(s));

let csrf = '', wifiId = '', firmwareId = '';
let ptzLease = '', ptzHeld = false, ptzStopping = false;
let audioClient = null, unbindTalk = null;
let videoPlayers = [];
let streams = [];
let fsOrigin = null;

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

function connection(state, label) {
  const pill = $('#connection');
  pill.className = 'pill' + (state === 'ok' ? '' : state === 'warn' ? ' warn' : ' crit');
  pill.innerHTML = '<i class="led"></i>';
  pill.append(label);
}

/* ---------- api ---------- */
class ApiError extends Error {
  constructor(message, kind) { super(message); this.kind = kind; }
}

async function api(path, options = {}) {
  options.headers = Object.assign({ Accept: 'application/json' }, options.headers || {});
  if (csrf && options.method && options.method !== 'GET') options.headers['X-CSRF-Token'] = csrf;
  let response;
  try {
    response = await fetch(path, options);
  } catch (_) {
    connection('crit', 'Camera not responding');
    throw new ApiError('The camera is not responding. It may be rebooting or off the network.', 'offline');
  }
  const type = response.headers.get('content-type') || '';
  const data = response.status === 204 ? null
    : type.includes('json') ? await response.json() : await response.text();
  if (!response.ok) {
    const isLogin = path === '/api/v1/session' && options.method === 'POST';
    if (response.status === 401 && !isLogin) requireLogin();
    throw new ApiError(data?.error?.message || data?.error || data || `HTTP ${response.status}`,
      response.status === 401 ? 'auth' : 'api');
  }
  connection('ok', 'Connected');
  return data;
}

function show(id) {
  for (const s of ['#login', '#setup', '#console']) $(s).classList.add('hidden');
  $(id).classList.remove('hidden');
}

function requireLogin() {
  videoPlayers.forEach(p => p.close());
  videoPlayers = [];
  if (audioClient) { unbindTalk?.(); audioClient.close().catch(() => {}); audioClient = null; }
  csrf = '';
  exitFullscreen(false);
  connection('warn', 'Signed out');
  show('#login');
}
window.addEventListener('joan-auth-required', requireLogin);

/* ---------- zones ---------- */
function setZone(zone) {
  $('#console').dataset.zone = zone;
  for (const b of all('#rail button')) b.setAttribute('aria-current', String(b.dataset.zone === zone));
  for (const s of all('.zone')) s.classList.toggle('hidden', s.id !== 'zone-' + zone);
}
for (const b of all('#rail button')) b.onclick = () => setZone(b.dataset.zone);

/* ---------- full screen ---------- */
function setFullscreen(which) {
  $('#console').dataset.fs = which;
  $('#fs-bar').classList.remove('hidden');
  for (const b of all('[data-fs-pick]')) b.classList.toggle('primary', b.dataset.fsPick === which);
  const stream = streams.find(s => s.id === which);
  $('#fs-sensor').innerHTML = '<i class="led"></i>';
  $('#fs-sensor').append(`${which} · ${stream ? stream.width + '×' + stream.height : ''}`);
}
function exitFullscreen(restoreFocus = true) {
  delete $('#console').dataset.fs;
  $('#fs-bar').classList.add('hidden');
  fsOrigin = null;
}
for (const tile of all('[data-fs]')) {
  tile.onclick = () => { fsOrigin = '#' + tile.id; setFullscreen(tile.dataset.fs); };
  /* A div with role=button does not fire click from Enter on its own. */
  tile.onkeydown = e => {
    if (e.key === 'Enter' || e.key === ' ') { e.preventDefault(); tile.click(); }
  };
}
for (const b of all('[data-fs-pick]')) b.onclick = () => setFullscreen(b.dataset.fsPick);
$('#fs-exit').onclick = () => exitFullscreen();

/* ---------- confirm guard ---------- */
/* Destructive actions get a second press, with Cancel focused. On a couch,
   with a remote, OK is the easiest button to hit by accident. */
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
const guardOpen = () => !!document.querySelector('.confirm');
function closeGuard() {
  for (const g of all('.confirm')) g.remove();
}

/* ---------- status ---------- */
const FEATURE_LABELS = {
  https_identity: ['HTTPS identity', 'device key'],
  rtsp: ['RTSP', 'digest auth'],
  fmp4_main: ['Main stream', null],
  fmp4_sub: ['Sub stream', null],
  ptz: ['Pan / tilt', null],
  audio: ['Audio', null],
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

const GLANCE = ['fmp4_main', 'fmp4_sub'];

function renderStatus(s) {
  const build = keys => keys.map(key => {
    const raw = s.features?.[key];
    if (raw === undefined) return null;
    const [label, fixed] = FEATURE_LABELS[key];
    const value = raw === true ? (fixed || 'on') : raw === false ? 'off' : String(raw);
    return chip(label, value, raw === false ? 'off' : '');
  }).filter(Boolean);
  const local = chip('Local only', s.local_only ? 'yes' : 'no', s.local_only ? '' : 'warn');
  $('#chips').replaceChildren(local, ...build(GLANCE));
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

  const stale = s.default_password_warning;
  $('#pw-warning').classList.toggle('hidden', !stale && s.ssh_password_sync);
  if (!s.ssh_password_sync) {
    $('#pw-warning').querySelector('.grow').textContent =
      'SSH password is not synchronized after upgrade. Change the administrator password once to enable it.';
  }
  $('#pw-state').textContent = stale ? 'initial value' : 'changed';
}

/* ---------- load ---------- */
async function loadStatus() {
  const s = await api('/api/v1/status');
  renderStatus(s);
  show('#console');
  return s;
}

async function load() {
  await loadStatus();
  const [streamList, mdns] = await Promise.all([
    api('/api/v1/streams'), api('/api/v1/network/mdns'),
  ]);
  streams = streamList.streams;
  $('#rail-host').textContent = mdns.address || '';
  $('#streams').replaceChildren(...streams.map(s => {
    const row = document.createElement('div');
    row.className = 'row';
    const k = document.createElement('span'); k.textContent = s.id;
    const v = document.createElement('b'); v.textContent = s.rtsp;
    row.append(k, v); return row;
  }));
  for (const s of streams) {
    const cap = $('#cap-' + s.id);
    if (cap) cap.textContent = `${s.id} · ${s.width}×${s.height}`;
  }
  videoPlayers.forEach(p => p.close());
  videoPlayers = streams.map(s => new Fmp4Player($('#video-' + s.id), s));
  videoPlayers.forEach(p => p.start().catch(x => notice(`${p.stream.id} video: ${x.message}`)));

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
  loadPresets().catch(() => {});
  loadCameraZone().catch(() => {});
}

/* ---------- auth ---------- */
$('#login-form').addEventListener('submit', async e => {
  e.preventDefault();
  try {
    const body = JSON.stringify(Object.fromEntries(new FormData(e.target)));
    const d = await api('/api/v1/session', {
      method: 'POST', headers: { 'Content-Type': 'application/json' }, body,
    });
    csrf = d.csrf;
    clearNotice();
    await load();
    if (d.default_password_warning) notice('The initial password is still active.', 'warn');
  } catch (x) { notice(x.message, 'crit'); }
});

$('#password-form').addEventListener('submit', async e => {
  e.preventDefault();
  try {
    await api('/api/v1/setup/password', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify(Object.fromEntries(new FormData(e.target))),
    });
    csrf = '';
    show('#login');
    notice('Password changed. Sign in again.');
  } catch (x) { notice(x.message, 'crit'); }
});

$('#change-password').onclick = () => show('#setup');

/* ---------- camera time ---------- */
/* The camera has no RTC and no NTP route, so its clock defaults years off,
   which shows in the burned-in OSD overlay and any recording metadata. Push
   this device's clock (true UTC) so it keeps correct time. The session is
   re-stamped server-side, so setting the clock does not sign the operator out. */
function tickLocalTime() { const el = $('#time-local'); if (el) el.textContent = new Date().toLocaleString(); }
setInterval(tickLocalTime, 1000); tickLocalTime();
async function setCameraTime(epochSeconds) {
  const d = await api('/api/v1/time', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ epoch: String(epochSeconds) }),
  });
  $('#time-camera').textContent = new Date(d.epoch * 1000).toLocaleString();
  $('#time-result').textContent = 'Camera clock set. Live video reloads to pick up the new timestamp.';
  load().catch(() => {});
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
  let name = '';
  try { name = Intl.DateTimeFormat().resolvedOptions().timeZone || ''; } catch (_) { /* no Intl */ }
  return { gmt_tz: `GMT${sign}${hh}:${mm}`, tz_name: name };
}
function renderCameraZone(label) {
  const el = $('#time-zone');
  if (el) el.textContent = label || '—';
}
async function loadCameraZone() {
  const d = await api('/api/v1/timezone');
  renderCameraZone(d.tz_name || d.gmt_tz || '—');
}
async function setCameraTimezone() {
  const { gmt_tz, tz_name } = browserTimeZone();
  return api('/api/v1/timezone', {
    method: 'PUT', headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify({ gmt_tz, tz_name }),
  });
}
$('#time-sync').onclick = async () => {
  try {
    await setCameraTime(Math.floor(Date.now() / 1000));
    const d = await setCameraTimezone();
    const zone = d.tz_name || d.gmt_tz;
    renderCameraZone(zone);
    $('#time-result').textContent =
      `Camera clock set to this device. Time zone saved as ${zone}; the on-screen overlay switches to it after the camera restarts.`;
  } catch (x) { notice(x.message, 'crit'); }
};
$('#time-set-manual').onclick = async () => {
  const v = $('#time-manual').value;
  if (!v) { notice('Choose a date and time first.'); return; }
  const epoch = Math.floor(new Date(v).getTime() / 1000);
  if (!Number.isFinite(epoch)) { notice('That date and time could not be read.'); return; }
  try { await setCameraTime(epoch); }
  catch (x) { notice(x.message, 'crit'); }
};
$('#goto-password').onclick = () => show('#setup');
$('#setup-cancel').onclick = () => show('#console');
$('#refresh').onclick = () => load().catch(x => notice(x.message, 'crit'));
$('#logout').onclick = async () => {
  try { await api('/api/v1/session', { method: 'DELETE' }); } catch (_) { /* ending anyway */ }
  requireLogin();
};

/* ---------- network ---------- */
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

/* ---------- ptz ---------- */
let jogSpeed = 3;
async function ptz(command) {
  if (!ptzLease) ptzLease = (await api('/api/v1/ptz/lease', { method: 'POST' })).lease;
  if (command === 'stop') {
    await api('/api/v1/ptz/stop', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ lease: ptzLease }),
    });
    ptzLease = '';
  } else {
    await api('/api/v1/ptz/move', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ lease: ptzLease, command, duration_ms: 500, speed: jogSpeed }),
    });
  }
  $('#ptz-state').textContent = ptzLease ? `Lease active: ${command}` : 'Stopped';
}
function releasePtz() {
  ptzHeld = false;
  if (!ptzLease || ptzStopping) return;
  ptzStopping = true;
  ptz('stop').catch(x => { ptzLease = ''; notice(x.message, 'crit'); })
    .finally(() => { ptzStopping = false; });
}
function startPtz(command) {
  ptzHeld = true;
  ptz(command).then(() => { if (!ptzHeld) releasePtz(); })
    .catch(x => { ptzLease = ''; notice(x.message, 'crit'); });
}

/* Pointer AND keyboard. A D-pad OK press emits keydown/keyup and a click;
   browsers do not synthesise pointer events for it, which is why the previous
   pointer-only binding left pan/tilt dead on a remote. `blur` is the deadman:
   a control that loses focus mid-press must not leave the motor running. */
for (const b of all('[data-ptz]')) {
  const command = b.dataset.ptz;
  if (command === 'stop') { b.onclick = releasePtz; continue; }
  const down = e => {
    e.preventDefault();
    if (e.pointerId !== undefined) {
      try { b.setPointerCapture?.(e.pointerId); } catch (_) { /* synthetic pointer */ }
    }
    startPtz(command);
  };
  const up = e => { e.preventDefault?.(); releasePtz(); };
  b.addEventListener('pointerdown', down);
  for (const name of ['pointerup', 'pointercancel', 'lostpointercapture']) {
    b.addEventListener(name, up);
  }
  b.addEventListener('keydown', e => {
    if ((e.code === 'Space' || e.code === 'Enter') && !e.repeat) down(e);
  });
  b.addEventListener('keyup', e => {
    if (e.code === 'Space' || e.code === 'Enter') up(e);
  });
  b.addEventListener('blur', up);
}

$('#speed-chips').replaceChildren(...[1, 2, 3, 4, 5].map(n => {
  const b = document.createElement('button');
  b.textContent = String(n);
  if (n === jogSpeed) b.classList.add('primary');
  b.onclick = () => {
    jogSpeed = n;
    for (const other of all('#speed-chips button')) other.classList.toggle('primary', other === b);
  };
  return b;
}));

async function operation(accepted) {
  for (let i = 0; i < 65; i++) {
    const state = await api(accepted.status);
    if (state.state === 'complete') return state.response;
    await new Promise(ok => setTimeout(ok, 1000));
  }
  throw new ApiError('Camera operation timed out', 'api');
}

let presets = [];
let selectedPreset = null;
async function loadPresets() {
  const response = await operation(await api('/api/v1/ptz/presets'));
  presets = response.ptz_coordinate || response.presets || [];
  $('#preset-chips').replaceChildren(...presets.map(item => {
    const token = String(item.coordinateID ?? item.token);
    const b = document.createElement('button');
    b.textContent = item.name || token;
    b.dataset.preset = token;
    if (token === selectedPreset) b.classList.add('primary');
    b.onclick = () => {
      selectedPreset = token;
      for (const other of all('#preset-chips button')) {
        other.classList.toggle('primary', other === b);
      }
      api('/api/v1/ptz/presets', {
        method: 'PUT', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ token: Number(token) }),
      }).then(operation).catch(x => notice(x.message, 'crit'));
    };
    return b;
  }));
  return presets;
}

$('#ptz-home').onclick = async () => {
  try {
    const list = await loadPresets();
    const home = list.find(x => x.name === '__home__');
    if (home) {
      await operation(await api('/api/v1/ptz/home', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'goto', token: Number(home.coordinateID ?? home.token) }),
      }));
    } else if (window.confirm('No home preset exists. Save the current position as home?')) {
      await operation(await api('/api/v1/ptz/home', {
        method: 'POST', headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ action: 'set' }),
      }));
    }
    notice('Home operation completed');
  } catch (x) { notice(x.message, 'crit'); }
};

$('#preset-set').onclick = async () => {
  try {
    const name = window.prompt('Preset name');
    if (!name) return;
    await operation(await api('/api/v1/ptz/presets', {
      method: 'POST', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ name }),
    }));
    await loadPresets();
  } catch (x) { notice(x.message, 'crit'); }
};

$('#preset-delete').onclick = () => {
  if (!selectedPreset) { notice('Choose a preset first.'); return; }
  const chosen = presets.find(p => String(p.coordinateID ?? p.token) === selectedPreset);
  guard($('#preset-confirm'), `Delete preset ${chosen?.name || selectedPreset}?`, 'Delete', () =>
    api('/api/v1/ptz/presets', {
      method: 'DELETE', headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ token: Number(selectedPreset) }),
    }).then(operation).then(() => { selectedPreset = null; return loadPresets(); })
      .catch(x => notice(x.message, 'crit')));
};

/* ---------- audio ---------- */
async function toggleAudio() {
  const button = $('#audio-connect');
  try {
    if (audioClient) {
      unbindTalk?.();
      await audioClient.close();
      audioClient = null;
      $('#push-to-talk').disabled = true;
      $('#audio-state').textContent = 'Disconnected';
      button.textContent = 'Listen';
      return;
    }
    const protocol = location.protocol === 'https:' ? 'wss:' : 'ws:';
    audioClient = new JooanAudioClient({
      url: `${protocol}//${location.host}/api/v1/audio/mic`,
      authToken: csrf,
      allowInsecure: location.protocol !== 'https:',
    });
    const connected = audioClient;
    audioClient.addEventListener('talkstate', e => {
      $('#audio-state').textContent = e.detail.active ? 'Speaking' : 'Listening';
    });
    audioClient.addEventListener('audioerror', e => notice(e.detail?.message || 'Audio error', 'crit'));
    audioClient.addEventListener('close', () => {
      if (audioClient !== connected) return;
      unbindTalk?.();
      audioClient = null;
      $('#push-to-talk').disabled = true;
      $('#audio-state').textContent = 'Disconnected';
      button.textContent = 'Listen';
    });
    await audioClient.connect();
    unbindTalk = audioClient.bindPressToTalk($('#push-to-talk'));
    $('#push-to-talk').disabled = false;
    $('#audio-state').textContent = 'Listening';
    button.textContent = 'Disconnect audio';
  } catch (x) {
    audioClient = null;
    $('#push-to-talk').disabled = true;
    notice(x.message, 'crit');
  }
}
$('#audio-connect').onclick = toggleAudio;
$('#audio-connect-2').onclick = toggleAudio;

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
  const session = await api('/api/v1/session');
  csrf = session.csrf;
  await load();
}
setZone('live');
resume().catch(x => {
  if (x.kind === 'offline') notice(x.message, 'crit');
  show('#login');
});
/* Retire any service worker installed by an earlier build; it cached index.html
   and app.js and would keep serving the pre-update UI after a firmware upgrade. */
if ('serviceWorker' in navigator) {
  navigator.serviceWorker.getRegistrations()
    .then(list => list.forEach(reg => reg.unregister())).catch(() => {});
}
if (window.caches) caches.keys().then(keys => keys.forEach(k => caches.delete(k))).catch(() => {});
