// SPDX-License-Identifier: MIT
// The admin UI's behaviours, on the mock: tooltips, the attention bar, the lock while an update is
// stopped, the folds, the choice of language. Starts its own mocks.   node states.mjs
import { launch, sleep } from './cdp.mjs';
import { spawn } from 'node:child_process';
import { mkdtempSync, writeFileSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const here = new URL('.', import.meta.url).pathname;
let port = 8190;
const mocks = [];
async function mock(...args) {
  const p = ++port, c = spawn('python3', [join(here, '../ui-mock-server.py'), '--port', p, ...args], { stdio: 'ignore' });
  mocks.push(c);
  const url = `http://127.0.0.1:${p}/`;
  for (let i = 0; i < 40; i++) { try { await fetch(url); break; } catch { await sleep(100); } }
  return url;
}
const bye = code => { mocks.forEach(c => c.kill()); process.exit(code); };
const b = await launch(9412, mkdtempSync(join(tmpdir(), 'ui-compare-')));
await b.send('Network.enable');
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: "window.__errs=[];addEventListener('error',e=>__errs.push(String(e.message)));addEventListener('unhandledrejection',e=>__errs.push('rejected: '+e.reason));document.addEventListener('securitypolicyviolation',e=>__errs.push('CSP '+e.violatedDirective))" });
const normal = await mock();
let fails = 0;
const ok = (c, m) => { console.log((c ? 'PASS ' : 'FAIL ') + m); if (!c) fails++; };
const E = s => b.eval(s);
const until = async (check, seconds) => { for (let i = 0; i < seconds * 4; i++) { try { if (await E(check)) return true; } catch {} await sleep(250); } return false; };
const vis = i => `(e=>!!e&&!e.hidden&&e.offsetHeight>0)(document.getElementById('${i}'))`;
async function signIn(url) {
  await b.viewport(1440, 900);
  await b.go(url, 800);
  await until("!!document.getElementById('si-form')&&!document.getElementById('signin').hidden", 15);
  await E(`document.getElementById('si-stay').checked=false;document.getElementById('si-pass').value='admin';document.getElementById('si-form').requestSubmit();1`);
  return until(vis('app'), 15);
}
const go = async p => { await E(`location.hash='#${p}';1`); await sleep(700); };
const errs = async w => { const e = await E('JSON.stringify(__errs.splice(0))'); ok(e === '[]', `no script errors or CSP reports (${w}) ${e === '[]' ? '' : e}`); };

ok(await signIn(normal), 'signs in');
// tooltips
const T = `document.querySelector('.page:not([hidden]) .tip')`;
ok(await E(`${T}.getAttribute('aria-expanded')=='false'&&${T}.nextElementSibling.hidden`), 'tooltip starts closed');
await E(`${T}.click();1`);
ok(await E(`${T}.getAttribute('aria-expanded')=='true'&&!${T}.nextElementSibling.hidden`), 'a click opens a tooltip');
ok(await E(`(r=>r.left>=0&&r.right<=innerWidth)(${T}.nextElementSibling.getBoundingClientRect())`), 'its note stays on the screen');
await E(`${T}.click();1`);
ok(await E(`${T}.nextElementSibling.hidden`), 'a second click closes it');
await E(`${T}.click();1`);
await E(`document.dispatchEvent(new KeyboardEvent('keydown',{key:'Escape',bubbles:true}));1`);
ok(await E(`${T}.nextElementSibling.hidden&&document.activeElement==${T}`), 'Escape closes it and returns focus');
await E(`${T}.click();document.querySelector('.page:not([hidden]) h1').click();1`);
ok(await E(`${T}.nextElementSibling.hidden`), 'a click elsewhere closes it');
const c = JSON.parse(await E(`JSON.stringify((r=>({x:r.x+r.width/2,y:r.y+r.height/2}))(${T}.getBoundingClientRect()))`));
await b.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: c.x, y: c.y });
await sleep(350);
ok(await E(`!${T}.nextElementSibling.hidden`), 'hovering with a mouse opens it');
await b.send('Input.dispatchMouseEvent', { type: 'mouseMoved', x: 700, y: 600 });
await sleep(350);
ok(await E(`${T}.nextElementSibling.hidden`), 'moving away closes it');
ok(await E(`!!document.getElementById('ov-prs').getAttribute('aria-describedby')&&!!document.getElementById(document.getElementById('ov-prs').getAttribute('aria-describedby'))`), 'the switch is described by its tooltip');
ok(await E(`[...document.querySelectorAll('.tip')].every(x=>x.tagName=='BUTTON'&&x.getAttribute('aria-label')&&x.getAttribute('aria-controls'))`), 'every tooltip is a labelled button');
// folds
ok(await E(`!document.getElementById('ov-fold').open`), 'Status is folded when all is normal');
await go('system');
ok(await E(`!document.getElementById('sy-fold').open`), 'Health is folded when all is normal');
// privacy
await go('overview');
await E(`document.getElementById('ov-prs').click();1`);
await sleep(200);
ok(await E(`document.getElementById('sheet').open`), 'turning on privacy asks first');
await E(`document.querySelector('#sheet form').requestSubmit();1`);
ok(await until(vis('at-priv'), 8), 'privacy banner appears');
await go('audio');
ok(await E(vis('at-priv')), 'privacy banner is on other pages too');
await go('pantilt');
ok(await E(`document.getElementById('pt-prs').getAttribute('aria-checked')=='true'`), 'both privacy switches agree');
await E(`document.getElementById('at-privb').click();1`);
ok(await until(`document.getElementById('at-priv').hidden`, 8), 'Turn off in the banner works');
ok(await E(`document.getElementById('ov-prs').getAttribute('aria-checked')=='false'&&document.getElementById('pt-prs').getAttribute('aria-checked')=='false'`), 'both switches follow');
// siren
await go('overview');
await E(`document.getElementById('ov-sir').click();1`);
await sleep(200);
ok(await E(`/nearby/.test(document.getElementById('sheet').textContent)`), 'the siren dialog says people nearby will hear it');
await E(`document.querySelector('#sheet form').requestSubmit();1`);
ok(await until(vis('at-sir'), 8), 'a sounding siren shows a banner');
await go('network');
ok(await E(vis('at-sir')), 'on other pages too');
await E(`document.getElementById('at-sirb').click();1`);
ok(await until(`document.getElementById('at-sir').hidden`, 8), 'Stop in the banner works');
// network
await sleep(300);
ok(await E(`document.getElementById('nw-scan').hidden`), 'scan is folded under Change network');
await E(`document.getElementById('nw-sb').click();1`);
ok(await until(vis('nw-nets'), 12), 'Change network scans');
await E(`document.getElementById('nw-hid').click();1`);
await sleep(200);
await E(`document.getElementById('nw-ssid').value='Café网络';document.getElementById('nw-psk').value='password1';document.getElementById('nw-join').requestSubmit();1`);
await sleep(300);
ok(await E(`/plain English/.test(document.getElementById('nw-join').textContent)`), 'a non-ASCII network name is refused with a plain reason');
await E(`document.getElementById('nw-jc').click();1`);
// updates
await go('system');
ok(await E(`document.getElementById('sy-upb').hidden&&document.getElementById('sy-mupb').hidden`), 'each package form is closed until a file is chosen');
const f = join(tmpdir(), 'pkg-test.bin'); writeFileSync(f, 'x');
const { root } = await b.send('DOM.getDocument');
const { nodeId } = await b.send('DOM.querySelector', { nodeId: root.nodeId, selector: '#sy-file' });
await b.send('DOM.setFileInputFiles', { files: [f], nodeId });
await sleep(300);
ok(await E(`!document.getElementById('sy-upb').hidden&&document.getElementById('sy-fn').textContent=='pkg-test.bin'`), 'choosing a file opens the form with its name');
ok(await E(`document.getElementById('sy-apply').disabled`), 'Install stays off until the upload is checked');
ok(await E(`/cut power/.test(document.getElementById('sy-upb').textContent)`), 'the form says not to cut power');
ok(await E(`!document.getElementById('sy-rs').disabled&&!document.getElementById('sy-rd').disabled&&document.getElementById('sy-lock').hidden`), 'Restart and Reset are open when nothing stopped');
// pan/tilt
await go('pantilt');
await until(vis('pt-view'), 6);
if (await E(vis('pt-view'))) {
  await E(`document.querySelector('#pt-view [value=aim]').click();1`);
  ok(await E(vis('pt-aim') + `&&document.getElementById('pt-vl').hidden`), 'the Aim tab swaps the picture');
  await E(`document.querySelector('#pt-view [value=live]').click();1`);
  ok(await E(vis('pt-vl')), 'and back');
} else console.log('SKIP the mock has no aim calibration');
await E(`document.getElementById('pt-save').click();1`);
await sleep(200);
ok(await E(`!!document.getElementById('sh-s')&&document.getElementById('sh-s').options.length>0`), 'Save offers the free slots');
await E(`document.getElementById('sheet').close();1`);
await errs('normal');
// offline
await fetch(new URL('/__mock/fail?on=1', normal));
ok(await until(vis('banner'), 20), 'offline banner appears');
ok(await E(`/answering/.test(document.getElementById('bn-t').textContent)&&document.querySelector('.page:not([hidden]) .card').inert`), 'it names the camera and the page is inert');
await fetch(new URL('/__mock/fail?on=0', normal));
await E(`document.getElementById('bn-try').click();1`);
ok(await until(`document.getElementById('banner').hidden`, 15), 'and goes when the camera answers');
await errs('offline');
// a stopped update
ok(await signIn(await mock('--update-status', 'failed:upgrade')), 'signs in with a stopped update');
ok(await until(vis('at-stop'), 6), 'the stopped update shows on the Overview, on opening');
ok(await E(`/SSH/.test(document.getElementById('at-stop').textContent)&&!!document.querySelector('#at-ssh code')`), 'with the SSH command to copy');
await go('system');
ok(await E(`document.getElementById('sy-rs').disabled&&document.getElementById('sy-rd').disabled&&!document.getElementById('sy-lock').hidden`), 'Restart and Reset are locked, and it says why');
ok(await E(`document.getElementById('sy-mapply').disabled`), 'media install stays off');
ok(await E(`/locked/.test(document.getElementById('at-stopp').textContent)`), 'the banner mentions the lock');
await errs('stopped');
ok(await signIn(await mock('--update-status', 'failed:upgrade,idle')), 'signs in while it is stopped, then repaired');
ok(await until(vis('at-stop'), 6), 'it shows first');
ok(await until(`document.getElementById('at-stop').hidden`, 40), 'and clears by itself once the camera says idle');
await go('system');
ok(await E(`!document.getElementById('sy-rs').disabled`), 'Restart opens again');
ok(await signIn(await mock('--update-status', 'failed:replay')), 'signs in after an old refusal');
await sleep(1500);
ok(await E(`document.getElementById('at-stop').hidden`), 'a refused package is old news: no banner');
ok(await signIn(await mock('--update-status', 'running')), 'signs in during a running install');
ok(await until(vis('at-inst'), 6), 'the install banner shows');
ok(await E(`/power/.test(document.getElementById('at-inst').textContent)`), 'it says not to cut power');
await errs('running');
ok(await signIn(await mock('--homing', '25')), 'signs in while calibrating');
ok(await until(`document.getElementById('ov-fold').open`, 6), 'Status opens by itself on a problem');
ok(await until(`!document.getElementById('ov-fold').open`, 40), 'and closes when it clears');
// language choice
for (const [al, want] of [['pt-PT,pt', 'pt-BR'], ['zh-TW,zh', 'en'], ['zh-CN', 'zh-CN'], ['uk', 'en'], ['de-AT', 'de'], ['es-MX', 'es'], ['fr-CA', 'fr'], ['ru', 'ru'], ['ja,en', 'en']]) {
  await b.send('Network.setUserAgentOverride', { userAgent: 'Mozilla/5.0 Chrome/140', acceptLanguage: al });
  await b.go(normal, 500);
  await E(`localStorage.removeItem('jipc-lang');1`);
  await b.go(normal, 800);
  ok((await E('document.documentElement.lang')) === want, `browser ${al} gets ${want}`);
}
await E(`localStorage.setItem('jipc-lang','"ru"');1`);
await b.go(normal, 800);
ok((await E('document.documentElement.lang')) === 'ru', 'a saved choice beats the browser');
ok(await until(`/Войти|Вход/.test(document.getElementById('si-h').textContent)`, 3), 'the page is in that language');
await E(`localStorage.removeItem('jipc-lang');1`);
console.log(fails ? `${fails} FAILED` : 'ALL PASS');
bye(fails ? 1 : 0);
