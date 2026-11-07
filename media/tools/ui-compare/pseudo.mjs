// SPDX-License-Identifier: MIT
// Open every page in a pseudo language (all translated text marked) and report text that was not
// translated: anything a person reads that _() and walk() do not reach. Data shows up too: names,
// numbers, the network list. Read the list.
//   python3 media/tools/ui-i18n.py --pseudo /tmp/ui
//   python3 media/tools/ui-mock-server.py --port 8094 --ui-dir /tmp/ui &
//   node media/tools/ui-compare/pseudo.mjs http://127.0.0.1:8094/
import { launch, sleep } from './cdp.mjs';
import { mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';
const url = process.argv[2];
const b = await launch(9411, mkdtempSync(join(tmpdir(), 'ui-pseudo-')));
await b.send('Security.setIgnoreCertificateErrors', { ignore: true });
await b.send('Page.addScriptToEvaluateOnNewDocument', { source: "try{localStorage.setItem('jipc-lang','\"es\"')}catch{};window.__errs=[];addEventListener('error',e=>__errs.push(String(e.message)));addEventListener('unhandledrejection',e=>__errs.push('rejected: '+e.reason))" });
await b.viewport(1440, 900);
await b.go(url, 2000);
const until = async (check, seconds) => { for (let i = 0; i < seconds * 2; i++) { try { if (await b.eval(check)) return true; } catch {} await sleep(500); } return false; };
await until("!!document.getElementById('si-form')&&!document.getElementById('signin').hidden", 20);
const scan = `(()=>{const out=new Set();const w=document.createTreeWalker(document.body,NodeFilter.SHOW_TEXT);
 const vis=e=>{for(;e&&e!==document.body;e=e.parentElement){if(e.hidden||e.matches('svg,code,pre,.mono,script,style,#st-lenses small,dialog:not([open])'))return false}return true};
 for(let n;(n=w.nextNode());){const t=n.data.trim();if(!t||!/[A-Za-z]{2}/.test(t))continue;if(!vis(n.parentElement))continue;
  const rest=t.replace(/⟦[^⟧]*⟧/g,'');if(/[A-Za-z]{2}/.test(rest))out.add(t)}
 for(const e of document.querySelectorAll('[aria-label],[placeholder]')){if(!vis(e))continue;for(const a of ['aria-label','placeholder']){const t=e.getAttribute(a);if(t&&/[A-Za-z]{2}/.test(t.replace(/⟦[^⟧]*⟧/g,'')))out.add(a+': '+t)}}
 return JSON.stringify([...out])})()`;
const seen = new Map();
const note = async where => { for (const t of JSON.parse(await b.eval(scan))) if (!seen.has(t)) seen.set(t, where); const e = await b.eval('JSON.stringify(__errs.splice(0))'); if (e !== '[]') console.log('ERRORS at', where, e); };
await note('signin');
await b.eval(`document.getElementById('si-stay').checked=false;document.getElementById('si-pass').value='admin';document.getElementById('si-form').requestSubmit();1`);
await until("(a=>a&&!a.hidden&&a.offsetHeight>0)(document.getElementById('app'))", 25);
await sleep(2000);
for (const [w, h] of [[1440, 900], [390, 844]]) {
  await b.viewport(w, h);
  for (const p of ['overview', 'pantilt', 'image', 'audio', 'streams', 'network', 'system', 'settings']) {
    await b.eval(`location.hash='#${p}';1`);
    await sleep(1500);
    await note(p + '@' + w);
    await b.eval(`document.querySelectorAll('.page:not([hidden]) details').forEach(d=>d.open=true);document.querySelectorAll('.page:not([hidden]) .tip').forEach(x=>x.nextElementSibling.hidden=false);1`);
    await note(p + ' opened@' + w);
    await b.eval(`document.querySelectorAll('.page:not([hidden]) .tipb').forEach(x=>x.hidden=true);1`);
  }
}
await b.viewport(1440, 900);
// dialogs and forms
const dialogs = [['overview', '#ov-sir'], ['overview', '#ov-prs'], ['pantilt', '#pt-rc'], ['pantilt', '#pt-save'], ['audio', '#au-on'], ['network', '#nw-sb'], ['network', '#nw-hid'], ['system', '#sy-rs'], ['system', '#sy-rd'], ['system', '#sy-pwa'], ['system', '#sy-pwm']];
for (const [p, sel] of dialogs) {
  await b.eval(`location.hash='#${p}';1`);
  await sleep(1000);
  await b.eval(`document.querySelector('${sel}').click();1`);
  await sleep(sel === '#nw-sb' ? 5000 : 500);
  await note(sel);
  await b.eval(`(d=>d.open&&d.close())(document.getElementById('sheet'));1`);
}
console.log(JSON.stringify([...seen], null, 0).replace(/\],\[/g, '],\n['));
process.exit(0);
