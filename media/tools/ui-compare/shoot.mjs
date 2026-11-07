// SPDX-License-Identifier: MIT
// Every page of the admin UI at both widths, from a mock or a camera.
//   node shoot.mjs URL OUT_DIR [PASSWORD]
// Writes OUT_DIR/<page>-desktop.png and -phone.png, and -dark.png versions of both, and fails if
// the app does not open, or on script errors, CSP reports or sideways scroll. Things fixed to the
// screen go back into the page: a picture taller than the screen would show them halfway down.
import { launch, sleep } from './cdp.mjs';
import { mkdirSync, mkdtempSync } from 'node:fs';
import { tmpdir } from 'node:os';
import { join } from 'node:path';

const [url, out, password = 'admin'] = process.argv.slice(2);
if (!url || !out) { console.error('usage: shoot.mjs URL OUT_DIR [PASSWORD]'); process.exit(2); }
mkdirSync(out, { recursive: true });
const b = await launch(9400, mkdtempSync(join(tmpdir(), 'ui-compare-')));
await b.send('Security.setIgnoreCertificateErrors', { ignore: true });
await b.send('Page.addScriptToEvaluateOnNewDocument', { source:
  "window.__errs=[];addEventListener('error',e=>__errs.push(String(e.message)));" +
  "addEventListener('unhandledrejection',e=>__errs.push('rejected: '+e.reason));" +
  "document.addEventListener('securitypolicyviolation',e=>__errs.push('CSP '+e.violatedDirective))" });
await b.viewport(1440, 900);
await b.go(url, 2000);
// A camera over HTTPS takes longer than a mock, so wait for each step rather than for a fixed time.
const until = async (check, seconds) => {
  for (let i = 0; i < seconds * 2; i++) {
    try { if (await b.eval(check)) return true; } catch { /* still navigating */ }
    await sleep(500);
  }
  return false;
};
await until("!!document.getElementById('si-form')&&!document.getElementById('signin').hidden", 20);
await b.eval(`document.getElementById('si-stay').checked=false;document.getElementById('si-pass').value=${JSON.stringify(password)};document.getElementById('si-form').requestSubmit();1`);
await until("(a=>a&&!a.hidden&&a.offsetHeight>0)(document.getElementById('app'))", 25);
await sleep(2000);
// A wrong password or a page that did not load would otherwise give clean pictures of the sign-in.
if (!await b.eval("(a=>a&&!a.hidden&&a.offsetHeight>0)(document.getElementById('app'))")) {
  console.error('the app did not open: check the URL and the password');
  process.exit(1);
}
const unfix = `document.querySelectorAll('body *').forEach(e=>{if(getComputedStyle(e).position==='fixed')e.style.setProperty('position','static','important')});1`;
const pages = ['overview', 'pantilt', 'image', 'audio', 'streams', 'network', 'system'];
let failed = 0, pictures = 0;
for (const [scheme, suffix] of [['light', ''], ['dark', '-dark']]) {
  await b.scheme(scheme);
  for (const [name, width, height] of [['desktop', 1440, 900], ['phone', 390, 844]]) {
    await b.viewport(width, height);
    for (const page of pages) {
      await b.eval(`location.hash='#${page}';1`);
      await sleep(1800);
      await b.eval(unfix);
      await b.shoot(join(out, `${page}-${name}${suffix}.png`));
      pictures++;
      const overflow = await b.eval('document.documentElement.scrollWidth-innerWidth');
      const errors = await b.eval('JSON.stringify(__errs.splice(0))');
      if (overflow > 0 || errors !== '[]') { failed = 1; console.log(`${page} at ${width}, ${scheme}: overflow ${overflow}, errors ${errors}`); }
    }
  }
}
console.log(failed ? 'problems above' : `${pictures} pictures, no script errors, no CSP reports, no sideways scroll`);
process.exit(failed);
