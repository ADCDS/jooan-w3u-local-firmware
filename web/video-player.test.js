import assert from 'node:assert/strict';

class Events {
  constructor() { this.listeners = new Map(); }
  addEventListener(name, fn) {
    const list = this.listeners.get(name) || [];
    list.push(fn);
    this.listeners.set(name, list);
  }
  removeEventListener(name, fn) {
    this.listeners.set(name, (this.listeners.get(name) || []).filter(item => item !== fn));
  }
  emit(name) { for (const fn of [...(this.listeners.get(name) || [])]) fn(); }
}
class SourceBuffer extends Events {
  constructor() {
    super();
    this.mode = 'segments';
    this.updating = false;
    this.buffered = { length: 0 };
    this.appended = [];
  }
  appendBuffer(bytes) {
    this.appended.push(bytes);
    queueMicrotask(() => this.emit('updateend'));
  }
  remove() { queueMicrotask(() => this.emit('updateend')); }
}
class MediaSourceMock extends Events {
  static instances = [];
  static isTypeSupported() { return true; }
  constructor() {
    super();
    this.readyState = 'closed';
    MediaSourceMock.instances.push(this);
    queueMicrotask(() => { this.readyState = 'open'; this.emit('sourceopen'); });
  }
  addSourceBuffer() { this.buffer = new SourceBuffer(); return this.buffer; }
  endOfStream() { this.readyState = 'ended'; }
}
globalThis.MediaSource = MediaSourceMock;
let objectUrls = 0;
const revoked = [];
globalThis.URL = {
  createObjectURL: () => `blob:${++objectUrls}`,
  revokeObjectURL: url => revoked.push(url),
};
const delays = [];
globalThis.setTimeout = (fn, ms) => { delays.push(ms); queueMicrotask(fn); return 1; };
globalThis.CustomEvent = class { constructor(type) { this.type = type; } };
const events = [];
globalThis.window = { MediaSource: MediaSourceMock, dispatchEvent: event => events.push(event.type) };
const { Fmp4Player } = await import('./video-player.js');
const stream = {
  mime: 'video/mp4; codecs="avc1.640032"', init: '/init.mp4', fragment: '/fragment.mp4',
};
const video = () => ({
  src: '', currentTime: 0, cleared: 0, loads: 0,
  removeAttribute() { this.src = ''; this.cleared++; },
  load() { this.loads++; },
  play: () => new Promise(() => {}),
});
const flush = async predicate => {
  for (let i = 0; i < 500 && !predicate(); i++) await new Promise(resolve => queueMicrotask(resolve));
  assert.ok(predicate(), 'player did not make expected progress');
};
const reply = (sequence, first, bytes = new ArrayBuffer(8)) => ({
  ok: true,
  arrayBuffer: async () => bytes,
  headers: {
    get: name => name === 'X-Joan-Sequence' ? sequence?.toString() ?? null :
      name === 'X-Joan-First-Sequence' ? first?.toString() ?? null : null,
  },
});
const error = status => ({ status, ok: false });
const fragmentAfter = url => Number(url.match(/[?&]after=(\d+)/)?.[1]);

// A batch may cover multiple consecutive fragments. Advance the cursor to
// the last included fragment, not the first, and keep legacy headers working.
{
  const requested = [];
  let player;
  window.fetch = async url => {
    if (url.includes('init.mp4')) return reply(0);
    const after = fragmentAfter(url);
    requested.push(after);
    if (requested.length === 1) return reply(25, 20);
    if (requested.length === 2) return reply(28, 26);
    if (requested.length === 3) return reply(29); // old single-fragment daemon
    return new Promise(() => {});
  };
  player = new Fmp4Player(video(), stream);
  await player.start();
  await flush(() => requested.length === 4);
  assert.deepEqual(requested, [0, 25, 28, 29]);
  assert.equal(player.sequence, 29);
  assert.equal(MediaSourceMock.instances.at(-1).buffer.appended.length, 4);
  player.close();
}

// A gap must not append discontinuous payload to the existing MSE. Stage
// fresh init + after=0 fragment before replacing the old video element src.
{
  const screen = video();
  let player, initCalls = 0, fragmentCalls = 0, releaseBootstrap;
  const bootstrap = new Promise(resolve => { releaseBootstrap = resolve; });
  const requests = [];
  window.fetch = async url => {
    if (url.includes('init.mp4')) { initCalls++; return reply(0); }
    const after = fragmentAfter(url);
    requests.push(after);
    fragmentCalls++;
    if (fragmentCalls === 1) return reply(25); // initial live-edge bootstrap
    if (fragmentCalls === 2) return reply(27); // sequence 26 is missing
    if (fragmentCalls === 3) { await bootstrap; return reply(32, 30); }
    return new Promise(() => {});
  };
  player = new Fmp4Player(screen, stream);
  await player.start();
  const oldSrc = screen.src;
  const oldBuffer = player.buffer;
  await flush(() => fragmentCalls === 3);
  assert.deepEqual(requests, [0, 25, 0]);
  assert.equal(initCalls, 2);
  assert.equal(screen.src, oldSrc, 'keep old video visible while bootstrap is unavailable');
  assert.equal(screen.cleared, 0);
  assert.equal(oldBuffer.appended.length, 2, 'discard discontinuous fragment');
  assert.equal(player.sequence, 25);
  releaseBootstrap();
  await flush(() => player.sequence === 32);
  assert.notEqual(screen.src, oldSrc);
  assert.equal(screen.cleared, 0, 'swap src without clearing old src first');
  await flush(() => requests.length === 4);
  assert.deepEqual(requests, [0, 25, 0, 32]);
  assert.equal(player.buffer.appended.length, 2, 'fresh init and staged fragment are appended');
  player.close();
}

// Server-side 409 at the running cursor and then repeated 409 at after=0:
// retry the *staging* bootstrap with capped backoff, never repeatedly blank
// or clear the already visible video. Once ready, commit the new timeline.
{
  const screen = video();
  let player, initCalls = 0, bootstrapCalls = 0, holdBootstrap;
  const bootstrapGate = new Promise(resolve => { holdBootstrap = resolve; });
  const waitsBefore = delays.length;
  const requests = [];
  window.fetch = async url => {
    if (url.includes('init.mp4')) { initCalls++; return reply(0); }
    const after = fragmentAfter(url);
    requests.push(after);
    if (after === 0) {
      bootstrapCalls++;
      if (bootstrapCalls === 1) return reply(7);
      if (bootstrapCalls <= 7) return error(409);
      await bootstrapGate;
      return reply(51, 50);
    }
    if (after === 7) return error(409);
    return new Promise(() => {});
  };
  player = new Fmp4Player(screen, stream);
  await player.start();
  const oldSrc = screen.src;
  const oldBuffer = player.buffer;
  await flush(() => bootstrapCalls === 8);
  assert.equal(initCalls, 8, 'fresh init precedes each bootstrap attempt');
  assert.equal(screen.src, oldSrc);
  assert.equal(screen.cleared, 0);
  assert.equal(screen.loads, 0);
  assert.equal(oldBuffer.appended.length, 2);
  assert.equal(player.sequence, 7);
  assert.deepEqual(delays.slice(waitsBefore), [1000, 2000, 4000, 8000, 8000, 8000]);
  holdBootstrap();
  await flush(() => player.sequence === 51);
  assert.notEqual(screen.src, oldSrc);
  assert.equal(screen.cleared, 0);
  await flush(() => requests.at(-1) === 51);
  player.close();
}

// Closing during a pending bootstrap aborts it without any late src swap or
// retry. A generation change similarly ignores an old response that arrives
// after a newer start() has installed its own MSE.
{
  const screen = video();
  let player, stagedSignal, releaseStage, fragmentCalls = 0;
  const stage = new Promise(resolve => { releaseStage = resolve; });
  window.fetch = async (url, options) => {
    if (url.includes('init.mp4')) return reply(0);
    fragmentCalls++;
    if (fragmentCalls === 1) return reply(4);
    if (fragmentCalls === 2) return error(409);
    if (fragmentCalls === 3) {
      stagedSignal = options.signal;
      return stage;
    }
    return new Promise(() => {});
  };
  player = new Fmp4Player(screen, stream);
  await player.start();
  await flush(() => Boolean(stagedSignal));
  player.close();
  assert.equal(stagedSignal.aborted, true);
  releaseStage(reply(13));
  await flush(() => screen.cleared === 1);
  assert.equal(screen.src, '');
  assert.equal(fragmentCalls, 3, 'closed player must not keep pumping');
}
{
  const screen = video();
  let player, releaseOldInit, initCalls = 0, fragmentCalls = 0;
  const oldInit = new Promise(resolve => { releaseOldInit = resolve; });
  window.fetch = async url => {
    if (url.includes('init.mp4')) {
      initCalls++;
      return initCalls === 1 ? oldInit : reply(0);
    }
    fragmentCalls++;
    return fragmentCalls === 1 ? reply(11) : new Promise(() => {});
  };
  player = new Fmp4Player(screen, stream);
  const oldStart = player.start();
  await flush(() => initCalls === 1);
  await player.start();
  const newSrc = screen.src;
  releaseOldInit(reply(0));
  await oldStart;
  await flush(() => player.sequence === 11);
  assert.equal(screen.src, newSrc, 'stale generation must not replace current media');
  assert.equal(MediaSourceMock.instances.at(-1).buffer.appended.length, 2);
  player.close();
}

// Transient errors still recover via staging after two consecutive failures;
// do not wait for play() to resolve before pumping the first fragment.
{
  let player, initCalls = 0, fragmentCalls = 0;
  window.fetch = async url => {
    if (url.includes('init.mp4')) { initCalls++; return reply(0); }
    fragmentCalls++;
    if (fragmentCalls <= 2) throw new Error('daemon restart');
    return reply(12);
  };
  player = new Fmp4Player(video(), stream);
  await player.start();
  await flush(() => initCalls === 2);
  await flush(() => player.sequence === 12);
  player.close();
}

// A newly opened player seeks to the latest buffered sample immediately.
{
  const liveVideo = video();
  let player;
  window.fetch = async url => url.includes('init.mp4') ? reply(0) : reply(35);
  player = new Fmp4Player(liveVideo, stream);
  await player.start();
  player.buffer.buffered = { length: 1, start: () => 0, end: () => 6 };
  await flush(() => liveVideo.currentTime === 5.3);
  player.close();
}

// Auth must dispatch the existing event and not turn a 401 into a retry storm.
{
  let player, fragmentCalls = 0;
  window.fetch = async url => {
    if (url.includes('init.mp4')) return reply(0);
    fragmentCalls++;
    return error(401);
  };
  player = new Fmp4Player(video(), stream);
  await player.start();
  await flush(() => events.includes('joan-auth-required'));
  assert.equal(fragmentCalls, 1);
  player.close();
}

console.log('video-player batches, staged recovery, cancellation, auth and live-edge: PASS');
