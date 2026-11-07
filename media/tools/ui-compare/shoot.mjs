// SPDX-License-Identifier: MIT
// Every page of the admin UI at both widths, from a mock or a camera.
//   node shoot.mjs URL OUT_DIR [PASSWORD]
// Writes OUT_DIR/<page>-desktop.png and -phone.png, and fails on script errors or CSP reports.
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
await b.eval(`document.getElementById('si-stay').checked=false;document.getElementById('si-pass').value=${JSON.stringify(password)};document.getElementById('si-form').requestSubmit();1`);
await sleep(6000);
const pages = ['overview', 'pantilt', 'image', 'audio', 'streams', 'network', 'system'];
let failed = 0;
for (const [name, width, height] of [['desktop', 1440, 900], ['phone', 390, 844]]) {
  await b.viewport(width, height);
  for (const page of pages) {
    await b.eval(`location.hash='#${page}';1`);
    await sleep(1800);
    await b.shoot(join(out, `${page}-${name}.png`));
    const overflow = await b.eval('document.documentElement.scrollWidth-innerWidth');
    const errors = await b.eval('JSON.stringify(__errs.splice(0))');
    if (overflow > 0 || errors !== '[]') { failed = 1; console.log(`${page} at ${width}: overflow ${overflow}, errors ${errors}`); }
  }
}
console.log(failed ? 'problems above' : `${pages.length * 2} pictures, no script errors, no CSP reports, no sideways scroll`);
process.exit(failed);
