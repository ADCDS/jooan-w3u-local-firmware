'use strict';
/* D-pad navigation for Android TV / Fire TV remotes.
 *
 * A remote delivers four arrows and an OK button: no pointer, no Tab. This
 * moves focus over whatever the page renders, reading the DOM's own geometry,
 * so a control that is on screen is a control the remote can reach. There is
 * no focus tree to declare and keep in sync.
 *
 * Inert unless the page looks like a TV, so arrow keys keep scrolling the
 * page everywhere else.
 */

const TV = /\b(?:AFT[A-Z0-9]*|TV|Tizen|Web0?S|BRAVIA)\b/i;
export const looksLikeTv = () =>
  /[?&]tv=1\b/.test(location.search) || TV.test(navigator.userAgent || '');

/* Arrow names, the short names older WebKit emits, and the Android key codes
 * some WebViews report instead of a usable `key`. */
const DIRS = { ArrowUp: 'up', ArrowDown: 'down', ArrowLeft: 'left', ArrowRight: 'right',
  Up: 'up', Down: 'down', Left: 'left', Right: 'right' };
const CODES = { 19: 'up', 20: 'down', 21: 'left', 22: 'right' };
export const direction = e => DIRS[e.key] || CODES[e.keyCode] || null;
export const isBack = e => e.key === 'Escape' || e.key === 'Backspace' || e.keyCode === 4;

/* OK/select. Do NOT read `e.code` here: the Android TV WebView reports it as
   an empty string, so a `code === 'Enter'` test never fires on the one device
   this is for. `key` is populated, and the keyCodes cover the WebViews that
   send the Android DPAD_CENTER (23) instead of Enter (13). */
export const isSelect = e =>
  e.key === 'Enter' || e.key === ' ' || e.key === 'Spacebar' ||
  e.keyCode === 13 || e.keyCode === 32 || e.keyCode === 23;

const SEL = 'a[href],button,input,select,textarea,[tabindex],[data-focusable]';

export function focusables(root) {
  return Array.from(root.querySelectorAll(SEL)).filter(el => {
    if (el.disabled || el.getAttribute('data-focusable') === 'off') return false;
    if (el.getAttribute('tabindex') === '-1' && !el.hasAttribute('data-focusable')) return false;
    const r = el.getBoundingClientRect();
    return r.width > 0 && r.height > 0 && getComputedStyle(el).visibility !== 'hidden';
  });
}

/* Pick the nearest focusable in the pressed direction.
 *
 * Two tiers. A candidate sharing the perpendicular axis — the same row for
 * left/right, the same column for up/down — is scored on travel distance
 * alone, so the ring lands on the NEAREST row and never skips one because the
 * row after it happens to be better centred. Everything else ranks behind.
 *
 * Left/right also refuse to leave their row: at the end of a row nothing
 * happens, rather than the ring jumping diagonally across the screen.
 * `escape` lets the zone rail opt out, since it must always have a way in.
 */
export function pick(from, list, dir, escape) {
  const a = from.getBoundingClientRect();
  const h = dir === 'left' || dir === 'right';
  let best = null, score = Infinity;
  for (const el of list) {
    if (el === from) continue;
    const b = el.getBoundingClientRect();
    const dx = (b.left + b.right) / 2 - (a.left + a.right) / 2;
    const dy = (b.top + b.bottom) / 2 - (a.top + a.bottom) / 2;
    const along = dir === 'left' ? -dx : dir === 'right' ? dx : dir === 'up' ? -dy : dy;
    if (along <= 1) continue;
    const across = h ? Math.abs(dy) : Math.abs(dx);
    const overlap = h ? Math.min(a.bottom, b.bottom) - Math.max(a.top, b.top)
      : Math.min(a.right, b.right) - Math.max(a.left, b.left);
    if (h && overlap <= 0 && !escape) continue;
    const s = overlap > 0 ? along + across / 1000 : 1e6 + along + across * 2;
    if (s < score) { score = s; best = el; }
  }
  return best;
}

/* Text fields and sliders own the axis they edit: left/right moves a caret or
 * a value, so the remote must not steal it. Up/down still moves focus out. */
const TEXT = ['text', 'password', 'search', 'url', 'email', 'tel', 'number'];
function ownsAxis(el, dir) {
  if (!el) return false;
  const h = dir === 'left' || dir === 'right';
  if (el.tagName === 'TEXTAREA') return true;
  if (el.tagName === 'SELECT') return !h;
  if (el.tagName !== 'INPUT') return false;
  const type = (el.type || 'text').toLowerCase();
  return type === 'range' ? h : TEXT.includes(type) ? h : false;
}

/* onBack returns true when it consumed the press; onCapture lets a focused
 * element take the arrows for itself (a full-screen picture driving pan). */
export function createNavigator({ root, onBack = () => false, onCapture = () => false }) {
  let ring = null;
  const list = () => focusables(root);

  function focus(el) {
    if (!el) return null;
    if (ring) ring.classList.remove('nav-focus');
    ring = el;
    el.classList.add('nav-focus');
    el.focus();   // the browser's own focus scrolling keeps the ring on screen
    return el;
  }

  function current() {
    if (ring && root.contains(ring) && ring.getBoundingClientRect().width > 0) return ring;
    return list()[0] || null;
  }

  function restore(selector) {
    const items = list();
    const want = selector ? root.querySelector(selector) : null;
    return focus((want && items.includes(want) ? want : null)
      || root.querySelector('[data-autofocus]') || items[0]);
  }

  function onKey(e) {
    if (e.defaultPrevented || e.altKey || e.ctrlKey || e.metaKey) return;
    const dir = direction(e);
    if (dir) {
      if (ownsAxis(document.activeElement, dir)) return;
      const from = current();
      if (from && from.hasAttribute('data-capture-arrows') && onCapture(from, dir)) {
        e.preventDefault();
        return;
      }
      e.preventDefault();
      if (from) {
        const next = pick(from, list(), dir, !!from.closest('[data-escapes-row]'));
        if (next) focus(next);
      } else restore();
      return;
    }
    if (isBack(e)) {
      /* The IME eats the first Back; this is the second. Leave the field
       * before handing Back to the page, so a text input is never a trap. */
      const el = document.activeElement;
      if (ownsAxis(el, 'left')) { el.blur(); focus(el); e.preventDefault(); return; }
      if (onBack()) e.preventDefault();
    }
  }

  /* Android TV opens the soft keyboard the moment focus enters a text field,
   * and the IME then swallows every D-pad key. That is correct while you are
   * typing an SSID, but the platform also moves focus on its own, which
   * desynchronises the ring. Track focus so the ring follows, and let Back
   * step out of the field rather than into the next one. */
  document.addEventListener('focusin', e => {
    const el = e.target;
    if (el === ring || !root.contains(el)) return;
    if (ring) ring.classList.remove('nav-focus');
    ring = el;
    el.classList.add('nav-focus');
  });

  document.addEventListener('keydown', onKey);
  return { focus, current, restore, list };
}
