import assert from 'node:assert/strict';
import { PtzSteps } from './ptz-steps.js';

globalThis.document = { hidden: false };
const tick = () => new Promise(resolve => setTimeout(resolve, 0));
const gate = () => { let done; return { promise: new Promise(resolve => { done = resolve; }), done: (...args) => done(...args) }; };

{
  const lease = gate(), calls = [], states = [];
  const control = new PtzSteps({ request: async (path, body) => {
    calls.push([path, body]);
    if (path.endsWith('/lease')) return lease.promise;
    return {};
  }, video: () => null, state: x => states.push(x), inputs: () => {}, settled: async () => false });
  const first = control.nudge('up');
  assert.equal(control.busy, true);
  await control.stop();             // press Stop before the network lease arrives
  lease.done({ lease: 'L' });
  await first;
  assert.deepEqual(calls.map(x => x[0]), ['/api/v1/ptz/lease', '/api/v1/ptz/stop']);
  assert.equal(control.lease, '');
  assert.equal(control.busy, false);
}

{
  const move = gate(), calls = [], states = [], input = [];
  let frame = 1;
  const control = new PtzSteps({ request: async (path, body) => {
    calls.push([path, body]);
    if (path.endsWith('/lease')) return { lease: 'L' };
    if (path.endsWith('/move')) return move.promise;
    return {};
  }, video: () => ({ readyState: 4, paused: false, getVideoPlaybackQuality: () => ({ totalVideoFrames: frame }) }),
  state: x => states.push(x), inputs: x => input.push(x), settled: async () => { frame++; return true; } });
  control.setMode('fine');
  const first = control.nudge('left');
  await tick();
  const second = control.nudge('right');
  assert.equal(await second, false, 'overlapping nudge must be ignored');
  const stop = control.stop();
  move.done({ ok: true, stopped: true });
  await stop;
  await first;
  assert.deepEqual(calls.map(x => x[0]), ['/api/v1/ptz/lease', '/api/v1/ptz/move', '/api/v1/ptz/stop']);
  assert.deepEqual(calls[1][1], { lease: 'L', command: 'left', duration_ms: 100, speed: 1 });
  assert.equal(control.lease, '');
  assert.equal(input.at(-1), false);
}

{
  const calls = [];
  const control = new PtzSteps({ request: async (path, body) => {
    calls.push([path, body]);
    if (path.endsWith('/lease')) return { lease: 'C' };
    if (path.endsWith('/move')) return { ok: true, stopped: true };
    return {};
  }, video: () => ({ readyState: 4, paused: false, getVideoPlaybackQuality: () => ({ totalVideoFrames: 10 }) }),
  state: () => {}, inputs: () => {}, settled: async () => false });
  control.setMode('coarse');
  assert.equal(await control.nudge('down'), false);
  assert.deepEqual(calls[1][1], { lease: 'C', command: 'down', duration_ms: 350, speed: 3 });
  assert.equal(calls.at(-1)[0], '/api/v1/ptz/move');
}
console.log('PTZ step controller races and bounded nudge modes: PASS');
