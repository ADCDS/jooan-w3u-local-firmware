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
  const p = ++port, c = spawn('python3', [join(here, '../ui-mock-server.py'), '--port', p, ...(process.env.UI_DIR ? ['--ui-dir', process.env.UI_DIR] : []), ...args], { stdio: 'ignore' });
  mocks.push(c);
  const url = `http://127.0.0.1:${p}/`;
  for (let i = 0; i < 40; i++) { try { await fetch(url); break; } catch { await sleep(100); } }
  return url;
}
const bye = code => { mocks.forEach(c => c.kill()); process.exit(code); };
const b = await launch(9412, mkdtempSync(join(tmpdir(), 'ui-compare-')));
await b.send('Network.enable');
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: "window.__errs=[];addEventListener('error',e=>__errs.push(String(e.message)));addEventListener('unhandledrejection',e=>__errs.push('rejected: '+e.reason));document.addEventListener('securitypolicyviolation',e=>__errs.push('CSP '+e.violatedDirective))" });
// What the mock cannot be told to do: answers set from the test, a dead camera, a clock moved on.
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: `(()=>{const D=Date.now;Date.now=()=>D()+(window.__skew||0);const f=window.fetch;window.__gets=0;const xs=XMLHttpRequest.prototype.send;XMLHttpRequest.prototype.send=function(b){window.__xs=(window.__xs||0)+1;return xs.call(this,b)};const xa=XMLHttpRequest.prototype.abort;XMLHttpRequest.prototype.abort=function(){if(!window.__noabort)return xa.call(this)};
 const J=(s,o)=>new Response(JSON.stringify(o),{status:s,headers:{'content-type':'application/json'}});
 window.fetch=async(u,o={})=>{u=String(u);const m=o.method||'GET';
  if(window.__off)throw new TypeError('off');
  if(window.__401)return J(401,{error:{code:'x'}});
  if(/joan.v1.update$/.test(u)){if(m=='GET'){window.__gets++;const a=window.__upd||JSON.parse(localStorage.getItem('__upd')||'null');if(window.__hold)await new Promise(r=>window.__release=r);if(a)return J(200,a)}if(m=='PUT'&&window.__put)return J(window.__put,{error:'no',reason:'wifi-trial'})}
  if(/restart-schedule$/.test(u)&&m=='GET')window.__sched=(window.__sched||0)+1;
  if(/network.mdns$/.test(u)&&localStorage.getItem('__nomdns'))return J(500,{error:'no'});
  if(/v1.controls?$/.test(u)&&m=='POST'&&window.__ctl)return J(500,{error:'boom'});
  const r=await f(u,o);
  if(/api.v1.status$/.test(u)&&window.__priv!==undefined){const d=await r.json();d.platform.privacy=window.__priv;return J(200,d)}
  return r}})()` });
const normal = await mock();
let fails = 0;
const ok = (c, m) => { console.log((c ? 'PASS ' : 'FAIL ') + m); if (!c) fails++; };
const E = s => b.eval(s);
const until = async (check, seconds) => { for (let i = 0; i < seconds * 4; i++) { try { if (await E(check)) return true; } catch {} await sleep(250); } return false; };
const vis = i => `(e=>!!e&&!e.hidden&&e.offsetHeight>0)(document.getElementById('${i}'))`;
async function signIn(url, before = '') {
  await b.viewport(1440, 900);
  await b.go(url, 800);
  await until("!!document.getElementById('si-form')&&!document.getElementById('signin').hidden", 15);
  before && await E(before + ';1');
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
ok(await E(`document.getElementById('sy-sum').textContent==''`), 'Health does not say All normal while it is away');
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
// a refused retry must not release the lock
const choose = async (sel, name = 'pkg-a.bin', text = 'x') => {
  const f = join(tmpdir(), name); writeFileSync(f, text);
  const { root } = await b.send('DOM.getDocument');
  const { nodeId } = await b.send('DOM.querySelector', { nodeId: root.nodeId, selector: sel });
  await b.send('DOM.setFileInputFiles', { files: [f], nodeId });
  await sleep(200);
};
ok(await signIn(await mock(), `window.__upd={state:'failed',reason:'upgrade'}`), 'a stopped update again');
await until(vis('at-stop'), 6);
await go('system');
ok(await E(`document.getElementById('sy-rss').disabled`), 'turning on the daily restart is held back too');
await until(`/./.test(document.getElementById('sy-pv').textContent)`, 4);
await choose('#sy-file');
await E(`document.getElementById('sy-stage').click();1`);
ok(await until(`!document.getElementById('sy-apply').disabled`, 8), 'a retry can be staged while stopped');
await E(`window.__skew=31e3;1`);
await sleep(3000);
ok(await E(`/./.test(document.getElementById('sy-ust').textContent)&&!document.getElementById('sy-ust').textContent.includes('SSH')&&!document.getElementById('sy-apply').disabled`), 'the 30 s check leaves a staged retry alone');
await choose('#sy-file', 'pkg-b.bin');
ok(await E(`document.getElementById('sy-apply').disabled`), 'choosing another file drops what was staged');
await E(`document.getElementById('sy-stage').click();1`);
await until(`!document.getElementById('sy-apply').disabled`, 8);
await E(`window.__put=409;document.getElementById('sy-apply').click();1`);
await sleep(200);
await E(`document.querySelector('#sheet form').requestSubmit();1`);
await until(`!!document.querySelector('#sheet .err')`, 5);
await E(`document.getElementById('sheet').close();1`);
await sleep(1200);
ok(await E(vis('at-stop') + `&&document.getElementById('sy-rs').disabled&&document.getElementById('sy-rd').disabled`), 'a refused retry keeps the bar and the lock');
ok(await E(`(document.querySelector('.state')||{}).textContent`) !== 'Online', 'the pill does not say Online');
await sleep(5500);
ok(await E(`document.getElementById('sy-sum').textContent==''`), 'Health does not say All normal under it');
await go('overview');
await sleep(5500);
ok(await E(`document.getElementById('ov-sum').textContent==''`), 'nor does Status');
await errs('refused retry');
// an answer that left before the retry began says nothing about it
ok(await signIn(await mock(), `window.__upd={state:'failed',reason:'upgrade'}`), 'a retry, with a check in flight');
await until(vis('at-stop'), 6);
await go('system');
await choose('#sy-file');
await E(`document.getElementById('sy-stage').click();1`);
await until(`!document.getElementById('sy-apply').disabled`, 8);
await E(`window.__hold=1;window.__skew=31e3;1`);
await sleep(1500);
await E(`document.getElementById('sy-apply').click();1`);
await sleep(200);
ok(await E(`/pkg-a.bin/.test(document.getElementById('sheet').textContent)`), 'the platform dialog names the file');
await E(`window.__upd={state:'running'};window.__hold=0;document.querySelector('#sheet form').requestSubmit();1`);
await sleep(100);
await E(`window.__release&&window.__release();1`);
await sleep(2500);
ok(await E(vis('at-inst')), 'the stale answer does not end the install');
ok(await E(`document.querySelector('.state').textContent`) !== 'Online', 'the pill does not say Online');
// choosing another file while one uploads
const slow = await mock('--latency', '1500');
ok(await signIn(slow), 'a slow upload');
await go('system');
await sleep(2500);
await choose('#sy-file', 'pkg-a.bin');
await E(`document.getElementById('sy-stage').click();1`);
await sleep(300);
await choose('#sy-file', 'pkg-b.bin');
await until(`document.getElementById('sy-stage').getAttribute('aria-busy')!='true'`, 20);
await sleep(300);
ok(await E(`document.getElementById('sy-apply').disabled&&document.getElementById('sy-fn').textContent=='pkg-b.bin'`), 'another file while uploading cancels the first');
// an upload the page can no longer abort, as when another upload has taken its handle
const both = await mock('--latency', '1500');
ok(await signIn(both), 'an upload out of reach');
await go('system');
await sleep(2500);
await choose('#sy-file', 'pkg-a.bin');
await E(`window.__xs=0;document.getElementById('sy-stage').click();1`);
await until(`window.__xs>=1`, 12);
await E(`window.__noabort=1;1`);
await choose('#sy-file', 'pkg-b.bin');
await until(`document.getElementById('sy-stage').getAttribute('aria-busy')!='true'`, 20);
await sleep(300);
ok(await E(`document.getElementById('sy-apply').disabled`), 'its answer does not leave the first file ready under the second one\'s name');
// the first check failed: ask again
ok(await signIn(await mock('--update-status', '500,failed:upgrade')), 'a first check with no answer');
await sleep(2000);
ok(await E(`document.getElementById('at-stop').hidden`), 'nothing to show yet');
await E(`window.__skew=31e3;1`);
ok(await until(vis('at-stop'), 6), 'the next check finds the stopped update');
// the schedule once per stop
ok(await signIn(await mock(), `window.__upd={state:'failed',reason:'upgrade'}`), 'a stopped update, checked again and again');
await until(vis('at-stop'), 6);
await sleep(1200);
await E(`window.__sched=0;window.__skew=31e3;1`);
await sleep(2200);
await E(`window.__skew=62e3;1`);
await sleep(2200);
ok((await E('window.__sched||0')) <= 1, `the schedule is not read on every check (${await E('window.__sched||0')})`);
// the daily restart under a stopped update
const dm = await mock();
ok(await signIn(dm), 'a daily restart');
await go('system');
await until(`document.getElementById('sy-rsn').textContent!=''`, 6);
await E(`document.getElementById('sy-rss').click();1`);
await sleep(800);
await E(`localStorage.setItem('__upd',JSON.stringify({state:'failed',reason:'upgrade'}));location.reload();1`);
await sleep(800);
await until(vis('at-stop'), 8);
ok(await until(`/daily/.test(document.getElementById('at-stopp').textContent)`, 8), 'the bar says it can still happen');
await E(`localStorage.removeItem('__upd');1`);
// a platform install that does not come back
ok(await signIn(await mock(), `window.__upd={state:'running'}`), 'a running install');
ok(await until(vis('at-inst'), 6), 'shows as installing');
ok(await E(`[...document.querySelectorAll('.lang')].every(x=>x.disabled)`), 'the language waits for it');
await E(`window.__off=1;window.__same=1;1`);
ok(await until(`!!document.querySelector('.page:not([hidden]) .card').inert`, 15), 'the camera goes away');
ok(await E(vis('at-inst') + `&&document.getElementById('banner').hidden`), 'an expected absence is not the red banner');
await E(`window.__skew=6*60e3;1`);
ok(await until(vis('at-late'), 20), 'after 5 minutes it says it has not come back');
ok(await until(vis('banner'), 3) && await E(`document.getElementById('at-inst').hidden&&!!document.querySelector('#at-ssh2 code')`), 'with the red banner, Try now and the SSH command');
await E(`window.__upd={state:'idle'};window.__off=0;1`);
ok(await until(`window.__same===undefined&&!!document.getElementById('si-form')`, 20), 'when it answers again, a new page');
// the watch runs once
ok(await signIn(await mock(), `window.__upd={state:'running'};location.hash='#system'`), 'a running install, opening on System');
await until(vis('at-inst'), 6);
await E(`window.__gets=0;1`);
await sleep(8200);
ok((await E('window.__gets')) <= 5, `one watch, not two (${await E('window.__gets')} reads in 8 s)`);
// a restart that takes too long
const rm = await mock();
ok(await signIn(rm), 'a restart');
await go('system');
await E(`document.getElementById('sy-rs').click();1`);
await sleep(200);
await E(`window.__off=1;document.querySelector('#sheet form').requestSubmit();1`);
ok(await until(vis('at-rest'), 6), 'an expected restart says so');
ok(await E(`document.getElementById('banner').hidden&&!!document.getElementById('at-restb')`), 'not the red banner, and Try now stays');
await E(`window.__skew=4*60e3;1`);
ok(await until(vis('banner') + `&&document.getElementById('at-rest').hidden`, 5), 'after 3 minutes it is simply not answering');
ok(await E(`document.querySelector('.state').textContent`) !== 'Restarting', 'and the pill agrees');
await E(`window.__off=0;window.__401=1;1`);
ok(await until(`!document.getElementById('signin').hidden`, 16), 'a session gone on its return signs out');
ok(await E(`[...document.querySelectorAll('#signin .lang')].every(x=>!x.disabled)`), 'and the language can be chosen there');
await E(`window.__401=0;1`);
// an install's silence is counted from when it fell silent
ok(await signIn(await mock(), `window.__upd={state:'running'}`), 'a long install');
await until(vis('at-inst'), 6);
await E(`window.__skew=14*60e3;1`);
await sleep(2500);
await E(`window.__off=1;1`);
await sleep(4500);
ok(await E(`document.getElementById('at-late').hidden`), 'its first silence is not already late');
await E(`window.__skew=20*60e3;1`);
ok(await until(vis('at-late'), 18), 'five minutes of silence is');
await E(`window.__off=0;1`);
ok(await until(`document.getElementById('at-late').hidden`, 16), 'an answer clears it');
await E(`location.hash='#system';1`);
await sleep(800);
ok(await E(`!/5 minutes/.test(document.getElementById('sy-ust').textContent)`), 'and the form no longer says so');
// the bar follows the camera on a page that does not otherwise read its status
ok(await signIn(await mock()), 'the bar on Network');
await go('network');
await E(`window.__priv=1;1`);
ok(await until(vis('at-priv'), 14), 'privacy started elsewhere appears');
await E(`window.__priv=0;1`);
ok(await until(`document.getElementById('at-priv').hidden`, 14), 'and goes');
// a failed action in the bar
await E(`window.__priv=1;1`);
await until(vis('at-priv'), 14);
await E(`window.__ctl=1;document.getElementById('at-privb').click();1`);
ok(await until(`!!document.querySelector('#at-priv .err')`, 5), 'a failed Turn off says so');
ok(await E(`!document.querySelector('#at-privb .err')`), 'beside the button, not inside it');
ok(await E(`document.querySelector('#at-priv>div').offsetWidth>100&&document.getElementById('at-priv').offsetHeight<300`), 'and the bar keeps its shape');
await E(`window.__priv=0;1`);
await until(`document.getElementById('at-priv').hidden`, 14);
await sleep(1100);
ok(await E(`!document.querySelector('#at-priv .err')`), 'the error goes when the bar does');
// the name that crosses to the camera
ok(await signIn(await mock(), `localStorage.setItem('__nomdns','1');localStorage.setItem('jipc-lang','"fr"')`), 'with no name from the camera');
await b.go(await E('location.href'), 500);
await until("!!document.getElementById('si-form')", 8);
await until(vis('app'), 15);
await go('streams');
await until(`/./.test(document.getElementById('st-env').textContent)`, 6);
ok(await E(`/FRIGATE_CAMERA_PASSWORD/.test(document.getElementById('st-env').textContent)`), 'a pasted config never holds a translated name');
ok(await E(`document.querySelector('#top .cam-name, #side .cam-name').textContent!='Camera'`), 'though the page shows one');
await E(`localStorage.removeItem('jipc-lang');localStorage.removeItem('__nomdns');1`);
// a pack that hangs
const hm = await mock();
await b.send('Fetch.enable', { patterns: [{ urlPattern: '*lang-ru.js*' }] });
await b.go(hm, 300);
await E(`localStorage.setItem('jipc-lang','"ru"');1`);
await b.go(hm, 300);
const t0 = Date.now();
ok(await until("!!document.getElementById('signin')&&!document.getElementById('signin').hidden&&document.documentElement.lang=='en'", 9), `a pack that hangs does not leave a blank page (${Date.now() - t0} ms), and the page is in English`);
await b.send('Fetch.disable');
await E(`localStorage.removeItem('jipc-lang');1`);
// the note of a page title's tooltip stays beside it
ok(await signIn(hm), 'tooltips again');
await E(`document.querySelector('.ph .tip').click();1`);
ok(await E(`document.querySelector('.ph .tipb').getBoundingClientRect().left>=document.querySelector('main').getBoundingClientRect().left`), 'a header tooltip opens in the page, not over the menu');
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
