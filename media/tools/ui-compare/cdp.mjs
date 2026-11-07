// SPDX-License-Identifier: MIT
// A headless Chromium over the DevTools protocol, with nothing to install but Chromium.
import { spawn } from 'node:child_process';
import { writeFileSync } from 'node:fs';

export const sleep = ms => new Promise(done => setTimeout(done, ms));

export async function launch(port, profile, chromium = process.env.CHROMIUM || 'chromium') {
  const browser = spawn(chromium, ['--headless=new', `--remote-debugging-port=${port}`, `--user-data-dir=${profile}`,
    '--no-first-run', '--no-default-browser-check', '--hide-scrollbars', 'about:blank'], { stdio: 'ignore' });
  process.on('exit', () => browser.kill());
  let target;
  for (let i = 0; i < 50 && !target; i++) {
    await sleep(200);
    try {
      const list = await (await fetch(`http://127.0.0.1:${port}/json`)).json();
      target = list.find(t => t.type === 'page');
    } catch (_) { /* not up yet */ }
  }
  if (!target) throw new Error('Chromium did not start');
  const socket = new WebSocket(target.webSocketDebuggerUrl);
  await new Promise((ok, no) => { socket.onopen = ok; socket.onerror = no; });
  let id = 0;
  const waiting = new Map();
  socket.onmessage = event => {
    const message = JSON.parse(event.data);
    const reply = waiting.get(message.id);
    if (!reply) return;
    waiting.delete(message.id);
    message.error ? reply.no(new Error(message.error.message)) : reply.ok(message.result);
  };
  const send = (method, params = {}) => new Promise((ok, no) => {
    waiting.set(++id, { ok, no });
    socket.send(JSON.stringify({ id, method, params }));
  });
  await send('Page.enable');
  await send('Runtime.enable');
  return {
    send,
    async go(url, wait = 1000) { await send('Page.navigate', { url }); await sleep(wait); },
    async viewport(width, height) {
      await send('Emulation.setDeviceMetricsOverride', { width, height, deviceScaleFactor: 1, mobile: width < 500 });
    },
    async scheme(value) { await send('Emulation.setEmulatedMedia', { features: [{ name: 'prefers-color-scheme', value }] }); },
    async eval(expression) {
      const r = await send('Runtime.evaluate', { expression, awaitPromise: true, returnByValue: true });
      if (r.exceptionDetails) throw new Error(r.exceptionDetails.exception?.description || r.exceptionDetails.text);
      return r.result.value;
    },
    async shoot(file) {
      const { data } = await send('Page.captureScreenshot', { format: 'png', captureBeyondViewport: true });
      writeFileSync(file, Buffer.from(data, 'base64'));
    }
  };
}
