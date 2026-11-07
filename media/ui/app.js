// SPDX-License-Identifier: MIT
// camera1 admin UI: vanilla JS, no build step, embedded in the media daemon
// (tools/check-size.py keeps it small). Platform calls go through the daemon's
// /api/joan/ relay; media calls go to /api/v1/.
'use strict';
/* Languages. English is in the code; a pack (lang-<code>.js, made by tools/ui-i18n.py from
   ui/i18n/<code>.json) maps a hash of each English string to its translation, so a reworded
   warning can never keep an old translation: it shows in English until someone translates it. */
const LANGS = { en: 'English', de: 'Deutsch (teilweise)', es: 'Español (parcial)', fr: 'Français (partiel)', 'pt-BR': 'Português do Brasil (parcial)', ru: 'Русский (частично)', 'zh-CN': '简体中文（部分）' },
	LANG_KEY = 'jipc-lang';
let PACK = null,
	LC = 'en',
	PR = null;
function langOf() {
	try {
		const v = JSON.parse(localStorage.getItem(LANG_KEY));
		if (LANGS[v]) return v;
	} catch {}
	for (const l of navigator.languages || []) {
		const x = l.toLowerCase(),
			b = x.split('-')[0];
		if (b == 'zh') return /^zh-(tw|hk|mo|hant)/.test(x) ? 'en' : 'zh-CN'; /* no Traditional pack yet */
		if (b == 'pt') return 'pt-BR';
		if (LANGS[b]) return b;
	}
	return 'en';
}
/* FNV-1a over UTF-16 units, base 36: tools/ui-i18n.py computes the same and refuses a collision. */
const ikey = t => {
	let x = 2166136261;
	for (let i = 0; i < t.length; i++) x = Math.imul(x ^ t.charCodeAt(i), 16777619);
	return (x >>> 0).toString(36);
};
const fill = (t, v) => (v ? t.replace(/\{(\w+)\}/g, (m, k) => (k in v ? v[k] : m)) : t);
function _(t, v) {
	return fill(PACK?.[ikey(t)] ?? t, v);
}
/* Plurals: the pack holds a word per CLDR category, under the hash of the English plural. */
function _n(n, one, other, v) {
	const p = PACK?.[ikey(other)],
		t = p == null ? (n == 1 ? one : other) : typeof p == 'string' ? p : (p[PR.select(n)] ?? p.other);
	return fill(t, { n, ...v });
}
/* The page's own text, once, before anything shows. */
function walk() {
	if (!PACK) return;
	const w = document.createTreeWalker(document.body, NodeFilter.SHOW_ELEMENT | NodeFilter.SHOW_TEXT, {
		acceptNode: e => (e.nodeType == 1 && e.matches('svg,code,pre,.mono') ? NodeFilter.FILTER_REJECT : NodeFilter.FILTER_ACCEPT)
	});
	for (let n; (n = w.nextNode()); )
		if (n.nodeType == 3) {
			const t = n.data.trim();
			if (t) n.data = n.data.replace(t, () => _(t));
		} else for (const a of ['aria-label', 'placeholder', 'data-t', 'data-l']) n.hasAttribute(a) && n.setAttribute(a, _(n.getAttribute(a)));
}
function main() {
	const $ = i => document.getElementById(i),
		$$ = (s, r = document) => [...r.querySelectorAll(s)],
		noop = () => {},
		J = '/api/joan/v1/',
		M = '/api/v1/';
	const tx = (i, t) => {
		const e = $(i);
		e.textContent != t && (e.textContent = t); /* no write, no announcement from a live region */
	},
		no = m => {
			throw new Fail(m);
		},
		bad = (el, m) => {
			el.setAttribute('aria-invalid', 'true');
			err(el, m);
		},
		act = (f, at) => e => run(e.currentTarget, f, at);
	function h(t, p, ...k) {
		const e = document.createElement(t);
		for (const a in p || {}) {
			const v = p[a];
			if (v == null || v === false) continue;
			if (a == 'text') e.textContent = v;
			else if (a[0] == 'o' && a[1] == 'n') e.addEventListener(a.slice(2), v);
			else e.setAttribute(a, v === true ? '' : v);
		}
		e.append(...k.flat().filter(x => x != null && x !== false));
		return e;
	}
	function ico(n) {
		const s = document.createElementNS('http://www.w3.org/2000/svg', 'svg');
		s.innerHTML = `<use href="#i-${n}"/>`;
		s.setAttribute('viewBox', '0 0 24 24');
		s.setAttribute('aria-hidden', 'true');
		return s;
	}
	const ls = {
		get(k) {
			try {
				return JSON.parse(localStorage.getItem(k));
			} catch {
				return null;
			}
		},
		set(k, v) {
			try {
				v == null ? localStorage.removeItem(k) : localStorage.setItem(k, JSON.stringify(v));
			} catch {}
		}
	};
	/* localStorage keys: per-browser preferences only */
	const THEME = 'jipc-theme',
		WIFI = 'jipc-wifi',
		SPEED = 'jipc-speed',
		LATCH = 'jipc-latch';
	let csrf = '',
		ep = 0,
		off = 0,
		route = '',
		user = 'admin',
		retry = 0,
		W = null,
		joining = 0,
		filled = 0,
		tzd = 0,
		pb = null,
		pt = 0,
		pq = Promise.resolve(),
		pn = 0,
		fast = 0,
		lastP = '',
		hs = '',
		fw = '',
		lens = 0,
		lastOk = 0, // when the camera last answered anything
		upx = null, // the platform upload in flight
		spw = null, // the stream password, fetched only when shown or copied
		reloadOn = 0, // an update: when the camera returns, the page may be a new one
		installing = 0, // one install at a time: the other's restart must not land in this one's flash
		instAt = 0, // when it began
		instLate = 0, // a media install that has not come back after 5 minutes
		restarting = '', // why the camera is away, when it was asked to go
		restAt = 0, // when
		fwWas = 0, // the install now running is a retry after one that stopped
		fwV = '', // what is staged, for the dialog
		fwW = 0, // a watch is running
		fwMsg = '', // an update needs a look, on every page until it has had one
		fwStopped = 0, // a platform install stopped: a media install ends in the restart that must not happen yet
		holdOff = 0, // after asking for a restart: answers until then do not mean it is back
		tzAt = 0; // after saving the time zone: the camera takes up to a minute to follow
	const S = {},
		age = k => Date.now() - (S[k + '@'] || 0);
	const p2 = n => String(n).padStart(2, '0'),
	mi = n => String(n).replace('-', '−'),
		sg = n => (n > 0 ? '+' : '') + mi(n),
		cap = s => (s ? s[0].toUpperCase() + s.slice(1) : '');
	const hm = d => d.toLocaleTimeString(LC);
	const un = (n, u, d) => new Intl.NumberFormat(LC, { style: 'unit', unit: u, unitDisplay: d || 'short' }).format(n),
		parts = (t, o) => t.split(/\{(\w+)\}/).map((x, i) => (i % 2 ? o[x] : x)),
		MODE = { day: _('Day'), night: _('Night'), auto: _('Auto'), schedule: _('Schedule') },
		BOOT = { native: _('Native'), handoff: _('Handoff') },
		ST = { running: _('Running'), ready: _('Ready'), idle: _('Idle'), talking: _('Talking'), starting: _('Starting'), stopped: _('Stopped'), stopping: _('Stopping'), off: _('Off'), unknown: _('Unknown') },
		stw = s => ST[s] || cap(s);
	function dur(s) {
		s = Math.max(0, Math.round(s));
		if (s < 60) return un(s, 'second');
		const m = (s / 60) | 0,
			H = (m / 60) | 0;
		return m < 60
			? un(m, 'minute')
			: H < 48
				? un(H, 'hour') + (m % 60 ? ' ' + un(m % 60, 'minute') : '')
				: un((H / 24) | 0, 'day') + (H % 24 ? ' ' + un(H % 24, 'hour') : '');
			}
	function bytes(n) {
		n = +n;
		if (!isFinite(n)) return _('Unknown');
		let i = 0;
		while (n >= 1e3 && i < 4) {
			n /= 1e3;
			i++;
		}
		return un(i ? +n.toFixed(n < 10 ? 2 : 1) : n, ['byte', 'kilobyte', 'megabyte', 'gigabyte', 'terabyte'][i]);
	}
	const sig = d => (d == null ? '' : d >= -50 ? _('Excellent') : d >= -60 ? _('Good') : d >= -70 ? _('Fair') : _('Weak'));
	const pill = (t, x) => h('span', { class: 'pill ' + t }, ico(t), x);
	let tt;
	function toast(m) {
		const t = $('toast');
		t.textContent = m;
		t.classList.add('show');
		clearTimeout(tt);
		tt = setTimeout(() => t.classList.remove('show'), 3200);
	}
	const host = e => e.closest('.sl,.row,.f,.acts,dialog,form,.card,.ab') || e;
	function clr(e) {
		$$(':scope>.err', host(e)).forEach(x => x.remove());
	}
	function err(e, m) {
		clr(e);
		host(e).append(h('p', { class: 'err', role: 'alert' }, ico('crit'), h('span', { text: m })));
	}
	const sb = e => e.submitter || e.target.querySelector('button:not([type=button])'),
		busy = e => e.getAttribute('aria-busy') == 'true',
		OFF = _('Not sent: the camera is not answering.');
	async function run(e, f, at) {
		if (!e || busy(e)) return;
		at = at || e;
		clr(at);
		e.setAttribute('aria-busy', 'true');
		e.setAttribute('aria-disabled', 'true');
		try {
			return await f();
		} catch (x) {
			if (x.kind == 'cancel') return;
			if (x.kind != 'auth' || x.message)
				err(at, x.kind == 'off' ? OFF : x.message || _('That did not work. Try again.'));
		} finally {
			e.removeAttribute('aria-busy');
			e.removeAttribute('aria-disabled');
		}
	}
	class Fail extends Error {
		constructor(m, kind, st, raw) {
			super(m);
			Object.assign(this, { kind, st, raw });
		}
	}
	const CODES = {
		invalid_credentials: _('That user name or password is not right.'),
		rate_limited: _('Too many tries. Wait a minute.'),
		csrf_rejected: _('Reload the page and try again.'),
		invalid_ntp: _('Use “gateway”, an IPv4 address (with an optional :port) or nothing.'),
		invalid_time: _('Pick a date between 2020 and 2100.'),
		invalid_hostname: _('Use 1–63 letters, digits or inner hyphens.'),
		invalid_routes: _('Only private networks can be added: 10.x, 172.16–31.x, 192.168.x or fd00::/8.'),
		password_rejected: _('The current password is wrong or the new one is not allowed.'),
		integration_failed: _('The camera could not finish that. Try again in a moment.')
	};
	const MED = [
		[/homing/, _('Pan/tilt is calibrating after start-up.')],
		[/already exists/, _('Another preset already has that name.')],
		[/slot is empty/, _('That preset slot is empty now.')],
		[/^audio is off/, _('Audio is off on the camera.')],
		[/another talker/, _('Someone else is talking through the camera.')],
		[/boot hook|muted until/, _('The speaker stays muted until start-up finishes.')],
		[/not ready|stopping/, _('The video service is starting. Try again shortly.')],
		[/[Ss]peaker/, _('The camera’s speaker is not available right now.')]
	];
	function say(st, code, raw) {
		if (CODES[code]) return CODES[code];
		for (const [r, m] of MED) if (r.test(raw)) return m;
		if (st == 429) return CODES.rate_limited;
		if (raw && raw.length < 140 && !/[{<]/.test(raw)) return PACK ? _('The camera reports: {m}', { m: raw }) : cap(raw).replace(/\.?$/, '.');
		return st >= 500 ? CODES.integration_failed : _('The camera refused that request.');
	}
	/* The media daemon relays at most two /api/joan/ requests at a time. */
	let jn = 0;
	const jq = [],
		take = () => (jn < 2 ? (jn++, Promise.resolve()) : new Promise(r => jq.push(r))),
		give = () => {
			const n = jq.shift();
			n ? n() : jn--;
		};
	async function req(p, o = {}) {
		const j = p.startsWith(J);
		j && (await take());
		const m = o.method || 'GET',
			hd = { 'X-Requested-With': 'jipc-ui', Accept: 'application/json' };
		let b = o.body;
		if (m != 'GET' && csrf) hd['X-CSRF-Token'] = csrf;
		if (o.json) {
			hd['Content-Type'] = 'application/json';
			b = JSON.stringify(o.json);
		} else if (o.type) hd['Content-Type'] = o.type;
		const ac = new AbortController(),
			t = setTimeout(() => ac.abort(), o.timeout || 1e4);
		try {
			const r = await fetch(p, { method: m, headers: hd, body: b, signal: ac.signal, cache: 'no-store' });
			lastOk = Date.now();
			return {
				r,
				d:
					r.status == 204
						? null
						: (r.headers.get('content-type') || '').includes('json')
							? await r.json().catch(() => null)
							: await r.text()
			};
		} catch {
			return null;
		} finally {
			clearTimeout(t);
			j && give();
		}
	}
	async function api(p, o = {}) {
		const e0 = ep,
			x = await req(p, o);
		if (!x) {
			o.quiet || goOff();
			throw new Fail(_('The camera is not answering.'), 'off');
		}
		if (e0 != ep && !o.login) throw new Fail('', 'auth');
		if (off && Date.now() > holdOff) setOn();
		const { r, d } = x;
		if (r.ok) return d;
		const e = d && d.error,
			code = (e && e.code) || '',
			raw = typeof e == 'string' ? e : (e && e.message) || (typeof d == 'string' ? d : '');
		if (r.status == 401 && !o.login) {
			out(_('Your session ended. Sign in again.'));
			throw new Fail('', 'auth');
		}
		if (r.status == 503 && /busy/.test(raw) && !o.bz) {
			await new Promise(z => setTimeout(z, 800));
			return api(p, { ...o, bz: 1 });
		}
		throw Object.assign(new Fail(say(r.status, code, raw), 'http', r.status, raw), { d });
	}
	const ctl = (operation, value = 0, value2 = 0, name) =>
		api(M + 'control', { method: 'POST', json: { operation, value, value2, name } });
	function src(k, p, f, o) {
		let q;
		return () =>
			q ||
			(q = api(p, o)
				.then(d => {
					S[k] = d;
					S[k + '@'] = Date.now();
					f(d);
					return d;
				})
				.finally(() => (q = null)));
	}
	const media = src('m', M + 'status', rMedia),
		config = src('c', M + 'config', rConfig),
		joan = src('j', J + 'status', rJoan),
		mdns = src('n', J + 'network/mdns', rName),
		quality = src('q', J + 'network/quality', rQuality, { timeout: 2e4 }),
		routes = src('r', J + 'network/routes', rRoutes, { timeout: 2e4 }),
		keys = src('k', J + 'ssh/authorized-keys', rKeys, { timeout: 2e4 }),
		clock = src('t', J + 'time', rTime),
		controls = src('x', M + 'controls', rControls),
		wifiState = src('w', J + 'network/wifi', rWifi, { quiet: 1, timeout: 8e3 }),
		restartAt = src('y', J + 'system/restart-schedule', rRestart, { timeout: 2e4 }),
		ipInfo = src('i', J + 'network/ip', rIp, { timeout: 2e4 });
	const guess = /^[\d.]+$|[:[]/.test(location.hostname) ? 'Camera' : location.hostname.split('.')[0],
		name = () => S.n?.hostname || guess, // sent to the camera and pasted in configs, so never translated
		label = () => (name() == 'Camera' ? _('Camera') : name()),
		rel = () => (/\/releases\/([0-9a-f]{8})/.exec(S.c?.libimp || '') || [])[1];
	function goOff(why) {
		if (!csrf || off) return;
		off = 1;
		retry = Date.now() + 1e4;
		restarting = why || '';
		restAt = Date.now();
		tx('bn-why', _('It may be restarting (about 90 s) or off the network. You are still signed in.'));
		tx('bn-last', lastOk ? _('Last answer {t}.', { t: hm(new Date(lastOk)) }) : '');
		ptzEnd(1);
		talkStop();
		place();
		shell();
		ov();
	}
	function setOn() {
		if (reloadOn) return location.reload();
		off = 0;
		restarting = '';
		place();
		shell();
		ov();
		load(route);
		W && !W.end && wifiState().catch(wifiLost);
	}
	async function probe() {
		retry = Date.now() + 1e4;
		if (Date.now() < holdOff) return;
		const e0 = ep,
			x = await req(J + 'session', { timeout: 6e3 });
		if (e0 != ep || !x) return;
		if (x.r.status == 401) return out(_('Your session ended. Sign in again.'));
		if (x.d?.csrf) csrf = x.d.csrf;
		setOn();
	}
	function place() {
		/* While the camera is away its controls are inert; the Wi-Fi countdown and
		   the last known status stay usable. */
		for (const c of $$('.page .card, .page>.f'))
			c.inert = off && !(W && c.contains($('nw-count'))) && c.id != 'ov-last';
	}
	$('bn-try').onclick = $('at-restb').onclick = e => run(e.currentTarget, probe);
	function note(m, t) {
		const n = $('si-note');
		n.hidden = !m;
		n.className = 'banner ' + (t || 'info');
		n.replaceChildren(ico(t ? 'crit' : 'ok'), h('span', { text: m || '' }));
	}
	function showSignIn(m, t) {
		$('app').hidden = true;
		$('signin').hidden = false;
		document.title = label() + ' · ' + _('Sign in');
		$('si-pass').value = '';
		note(m, t);
		req(J + 'setup/status', { timeout: 6e3 }).then(x => {
			const d = x?.r.ok && x.d,
				f = $('si-first');
			f.hidden = !d?.default_password_warning;
			f.textContent = _('Initial value in use: {a} / admin.', { a: d?.temporary_user || 'admin' });
		});
	}
	const NA = _('The camera is not answering. It may be restarting (about 90 s) or off the network.');
	async function signIn(c) {
		const e0 = ++ep;
		csrf = '';
		let d;
		try {
			d = await api(J + 'session', { method: 'POST', json: c, login: 1, quiet: 1, timeout: 15e3 });
		} catch (x) {
			if (x.kind == 'off') no(NA);
			x.st == 401 && $('si-pass').setAttribute('aria-invalid', 'true');
			throw x;
		}
		if (e0 != ep) return;
		if (!d?.csrf) no(_('No session was started. Try again.'));
		csrf = d.csrf;
		user = c.username;
		enter();
	}

	$('si-form').onsubmit = e => {
		e.preventDefault();
		const u = $('si-user').value.trim(),
			p = $('si-pass').value;
		if (!u || !p) return err($('si-pass'), _('Enter the user name and the password.'));
		/* "Stay signed in" is the camera's 30-day cookie; the password is never kept here. */
		const c = { username: u, password: p, ...($('si-stay').checked ? { remember: true } : {}) };
		run($('si-go'), () => signIn(c), $('si-pass'));
	};
	function out(m) {
		if (reloadOn) return location.reload(); // a restart ends a session too: sign in on the new page
		++ep;
		csrf = '';
		off = 0;
		for (const s of Object.values(STL)) {
			s.url && URL.revokeObjectURL(s.url);
			delete s.url;
			delete s.at;
			delete s.tried;
		}
		$$('.still img').forEach(i => i.remove());
		ptzEnd(1);
		talkStop();
		hidePw();
		place();
		showSignIn(m);
		$('si-h').focus();
	}
	$$('.signout').forEach(
		b =>
			(b.onclick = () =>
				run(b, async () => {
					const e0 = ++ep;
					await req(J + 'session', { method: 'DELETE', timeout: 5e3 });
					e0 == ep && out(_('You are signed out.'));
				}))
	);
	function enter() {
		$('signin').hidden = true;
		$('app').hidden = false;
		$$('.signed').forEach(e => e.replaceChildren(...parts(_('Signed in as {user}'), { user: h('b', { class: 'ink', text: user }) })));
		fwLast().catch(noop);
		W = ls.get(WIFI);
		if (W && !(W.deadline > Date.now())) ls.set(WIFI, (W = null));
		wifi();
		[config, mdns, media, joan].forEach(f => f().catch(noop));
		route = '';
		go(1);
		/* A trial started anywhere needs an answer in time: bring its countdown up on any device. */
		wifiState()
			.then(() => W && !W.end && route != 'network' && (location.hash = '#network'))
			.catch(wifiLost);
	}
	const TI = {
			overview: _('Overview'),
			pantilt: _('Pan/tilt'),
			image: _('Image & night'),
			audio: _('Audio'),
			streams: _('Streams & integrations'),
			network: _('Network'),
			system: _('System'),
			settings: _('Settings')
		},
		SUB = ['streams', 'network', 'system'],
		wide = matchMedia('(min-width:900px)');
	const title = () => csrf && (document.title = label() + ' · ' + TI[route]);
	function go(f) {
		let r = location.hash.slice(1);
		if (!TI[r]) r = 'overview';
		if (r == 'settings' && wide.matches) history.replaceState(null, '', '#' + (r = 'streams'));
		if (r != route) {
			talkStop();
			ptzEnd();
			closeMenu();
			hidePw();
			filled = 0;
		}
		route = r;
		$$('.page').forEach(p => (p.hidden = p.id != 'p-' + r));
		const tab = SUB.includes(r) ? 'settings' : r;
		for (const a of $$('#side a,#tabs a')) {
			const k = a.hash.slice(1);
			(a.closest('#tabs') ? k == tab : k == r)
				? a.setAttribute('aria-current', 'page')
				: a.removeAttribute('aria-current');
		}
		$('top').classList.toggle('sub', SUB.includes(r));
		place();
		title();
		r == 'system' && seed();
		f &&
			$('p-' + r)
				.querySelector('h1')
				.focus();
		scrollTo(0, 0);
		load(r);
		stShow();
		stTick();
	}
	onhashchange = () => csrf && go(1);
	wide.onchange = () => route == 'settings' && go();
	const LD = {
		overview: [media, joan, quality, controls],
		audio: [media, config, controls],
		pantilt: [media, [controls, 'pt-ptst'], aim],
		image: [media, config, [controls, 'im-nst']],
		streams: [config, mdns, media, controls],
		network: [[quality, 'nw-now'], mdns, [routes, 'nw-routes'], wifiState, [ipInfo, 'nw-ipm']],
		system: [joan, media, config, controls, fwLast, [clock, 'sy-nh'], [restartAt, 'sy-rsn'], [keys, 'sy-keys']],
		settings: [joan, media, quality]
	};
	function load(r) {
		for (const x of LD[r] || []) {
			const [f, at] = [].concat(x);
			f().catch(e => e.kind == 'http' && at && err($(at), e.message));
		}
	}
	function pick(el, v) {
		const bs = $$('button', el);
		let any;
		for (const b of bs) {
			const on = b.value === String(v);
			any ||= on;
			b.setAttribute('role', 'radio');
			b.setAttribute('aria-checked', on);
			b.tabIndex = on ? 0 : -1;
		}
		if (!any && bs[0]) bs[0].tabIndex = 0;
	}
	function seg(el, f) {
		el.addEventListener('click', e => {
			const b = e.target.closest('button');
			b && b.getAttribute('aria-checked') != 'true' && f(b.value);
		});
		el.addEventListener('keydown', e => {
			const d = { ArrowRight: 1, ArrowDown: 1, ArrowLeft: -1, ArrowUp: -1 }[e.key],
				bs = $$('button', el),
				i = bs.indexOf(document.activeElement);
			if (!d || i < 0) return;
			e.preventDefault();
			e.stopPropagation();
			const n = bs[(i + d + bs.length) % bs.length];
			n.focus();
			f(n.value);
		});
	}
	const sw = (b, on) => b.setAttribute('aria-checked', !!on),
		SL = {};
	function slider(id, label, min, max, v, fmt, set) {
		const i = h('input', { id, type: 'range', min, max, value: v }),
			o = h('output', { for: id, class: 'mono', text: fmt(v) }),
			row = h('div', { class: 'sl' }, h('div', null, h('label', { for: id, text: label }), o), i);
		let t,
			b = 0,
			nx = null;
		const send = async () => {
			if (b) return;
			b = 1;
			while (nx != null) {
				const x = nx;
				nx = null;
				clr(i);
				try {
					await set(x);
				} catch (e) {
					e.kind != 'auth' && err(i, e.kind == 'off' ? OFF : e.message);
				}
			}
			b = 0;
		};
		i.oninput = () => {
			o.textContent = fmt(+i.value);
			clearTimeout(t);
			t = setTimeout(() => {
				nx = +i.value;
				send();
			}, 250);
		};
		SL[id] = { row, i, o, fmt, idle: () => !b && nx == null && document.activeElement != i };
		return row;
	}
	function setSl(id, v, force) {
		const s = SL[id];
		s.row.hidden = v == null;
		if (v != null && (force || s.idle())) {
			s.i.value = v;
			s.o.textContent = s.fmt(v);
		}
	}
	function cf(t, label) {
		const c = h('code', { text: t }),
			b = h('button', { 'aria-label': _('Copy: {w}', { w: label }) }, ico('copy'), h('span', { text: _('Copy') }));
		b.onclick = () => copy(t, b, c);
		return h('div', { class: 'cf' }, c, b);
	}
	/* Copy to the clipboard; without one, select the visible text (el) or show it to copy by hand. */
	async function copy(t, b, el) {
		let ok;
		try {
			await navigator.clipboard.writeText(t);
			ok = 1;
		} catch {
			if (!el) {
				const pre = h('pre', { tabindex: 0, text: t });
				sheet({ title: _('Copy this text'), icon: 'copy', body: [pre], ok: _('Done'), run: noop, focus: pre });
				return getSelection().selectAllChildren(pre);
			}
			getSelection().selectAllChildren(el);
		}
		const sp = b.querySelector('span');
		sp.textContent = ok ? _('Copied') : _('Selected');
		b.classList.toggle('done', !!ok);
		clearTimeout(b.t);
		b.t = setTimeout(() => {
			sp.textContent = _('Copy');
			b.classList.remove('done');
		}, 2e3);
	}
	let uid = 0;
	function pwf(label, ac) {
		const i = h('input', { type: 'password', autocomplete: ac, id: 'p' + ++uid, maxlength: 128 });
		return { i, f: fld(label, i) };
	}
	document.addEventListener('click', e => {
		const b = e.target.closest('.reveal');
		if (!b) return;
		const i = b.previousElementSibling,
			on = i.type == 'password';
		i.type = on ? 'text' : 'password';
		b.setAttribute('aria-pressed', on);
		b.setAttribute('aria-label', on ? _('Hide password') : _('Show password'));
	});
	document.addEventListener('input', e => {
		const t = e.target;
		if (t.hasAttribute?.('aria-invalid')) {
			t.removeAttribute('aria-invalid');
			clr(t);
		}
	});
	function sheet(o) {
		const d = $('sheet'),
			no = h('button', { type: 'button', class: 'btn', onclick: () => d.close() }, _('Cancel')),
			ok = h('button', { class: 'btn ' + (o.danger ? 'dzf' : 'pri') }, o.ok),
			f = h(
				'form',
				{ class: 'stack s', novalidate: true },
				o.body,
				h('div', { class: 'acts' }, no, ok),
				o.extra
			);
		f.onsubmit = e => {
			e.preventDefault();
			run(
				ok,
				async () => {
					await o.run();
					d.open && d.close();
				},
				f
			);
		};
		d.replaceChildren(
			h(
				'div',
				{ class: 'sh-h' },
				h('span', { class: 'ico ' + (o.danger ? 'crit' : 'wait') }, ico(o.icon || 'warn')),
				h('h2', { id: 'sh-h', text: o.title })
			),
			o.text && h('p', { class: 'muted', id: 'sh-p', text: o.text }),
			f
		);
		o.text ? d.setAttribute('aria-describedby', 'sh-p') : d.removeAttribute('aria-describedby');
		d.showModal();
		(o.focus || no).focus();
	}
	const fld = (l, i) => h('div', { class: 'f' }, h('label', { for: i.id, text: l }), i);
	function shell() {
		const m = S.m,
			st = fwStopped
				? ['crit', _('Update stopped')]
				: installing
					? ['warn', _('Installing')]
					: off
				? restarting
					? ['warn', _('Restarting')]
					: ['crit', _('Not answering')]
				: sirOn()
					? ['crit', _('Siren sounding')]
				: W && !W.end
					? ['warn', _('Testing a new network')]
					: !m
						? ['off', _('Connecting')]
						: m.ready
							? m.platform?.privacy
								? ['warn', _('Privacy mode')]
								: ['ok', _('Online')]
							: ['warn', _('Starting')],
			/* the side nav also names the boot mode; a phone header has room for the state only */
			a = S.q?.wifi?.address || location.hostname;
		$$('.cam-name').forEach(e => (e.textContent = label()));
		$$('.cam-addr').forEach(e => (e.textContent = a));
		$$('.state').forEach(e => e.replaceChildren(pill(st[0], st[1])));
		tx('sy-cmd', `ssh ${user}@${a}`);
		attn();
		tx('se-ver', _('JOOAN W3-U · platform {p} · media {m}', { p: S.j?.version || '…', m: rel() || m?.version || '…' }));
		title();
	}
	function rMedia(m) {
		shell();
		ov();
		pos();
		presets();
		lights();
		audio();
		tx('sy-mv', rel() ? _('{v} · release {r} installed', { v: m.version, r: rel() }) : _('{v} installed', { v: m.version }));
	}
	function rConfig() {
		shell();
		streams();
		orient();
		audio();
		ov();
	}
	function rJoan(j) {
		shell();
		ov();
		tx('sy-pv', _('{v} installed', { v: j.version }));
		tx(
			'sy-pws',
			j.default_password_warning ? _('This page and SSH, user {u}. Initial value.', { u: user }) : _('This page and SSH, user {u}. Changed.', { u: user })
		);
		$('sy-sync').hidden = j.ssh_password_sync !== false;
	}
	function rName(n) {
		shell();
		streams();
		document.activeElement != $('nw-host') && ($('nw-host').value = n.hostname);
	}
	function spec(ch) {
		const s = S.m?.streams?.find(x => x.channel == ch),
			c = S.c;
		return s
			? `${s.width} × ${s.height}` +
					(c ? ` · ${c.fps} fps · H.264 · ${ch ? c.bitrate1 : c.bitrate} kbit/s` : '')
			: '';
	}
	function ntp(n) {
		return !n?.server
			? [_('Off')]
			: n.synchronized
				? [_('Synced'), _('Synced {t} ago', { t: dur(n.last_sync_age_seconds + age('j') / 1e3) })]
				: [_('Not synced')];
	}
	/* Status rows [tone, label, value]. `sys`: the System page's health list, which adds the
	   watchdog and the board's state and leaves audio and Wi-Fi to their own pages. A missing
	   status.system (older firmware) hides the rows that need it. */
	function hl(sys) {
		const m = S.m,
			j = S.j,
			w = S.q?.wifi,
			L = [],
			a = (t, k, v) => L.push([t, k, v]),
			kb = n => bytes(n * 1024);
		if (m) {
			const p = m.platform || {},
				A = m.audio || {},
				mc = A.microphone || {},
				sp = A.speaker || {},
				y = m.system || {};
			a(
				m.ready ? 'ok' : m.backend_error ? 'crit' : 'warn',
				_('Media service'),
				m.ready ? _('Running · {t}', { t: dur(m.uptime_seconds) }) : m.backend_error || _('Starting')
			);
			m.restart_pending && a('warn', _('Video'), _('Restarting'));
			a(
				!p.homing && !p.last_error && p.position_known ? 'ok' : 'warn',
				_('Pan/tilt'),
				p.homing ? _('Calibrating') : p.last_error || (p.position_known ? _('Ready') : _('Position unknown'))
			);
			sys && S.c && a(S.c.watchdog ? 'ok' : 'off', _('Watchdog'), S.c.watchdog ? _('Active') : _('Not used'));
			if (sys) {
				y.boot_mode && y.boot_mode != 'unknown' && a('ok', _('Boot mode'), BOOT[y.boot_mode] || cap(y.boot_mode));
				y.memfree_kb != null && a(y.memfree_kb < 1024 ? 'warn' : 'ok', _('Memory free'), kb(y.memfree_kb));
				if (y.state_free_kb != null) {
					const low = y.state_free_kb < 64; // the settings partition is 384 KB, and updates need this much left
					a(low ? 'warn' : 'ok', _('Flash free'), low ? _('{n}, low', { n: kb(y.state_free_kb) }) : kb(y.state_free_kb));
				}
			} else if (y.uptime_s != null) {
				const since = new Date(Date.now() - y.uptime_s * 1e3);
				a(
					'ok',
					_('Uptime'),
					_('{t} · since {w}', { t: dur(y.uptime_s), w: since.toLocaleString(LC, { day: 'numeric', month: 'short', hour: 'numeric', minute: '2-digit' }) })
				);
			}
			if (sys) {
				/* audio has its own page */
			} else if (!A.enabled) a('off', _('Audio'), _('Off'));
			else {
				a(
					mc.error || mc.state != 'running' ? 'warn' : A.muted ? 'off' : 'ok',
					_('Microphone'),
					mc.error || (A.muted ? _('Off (sends silence)') : stw(mc.state || 'unknown'))
				);
				a(
					sp.error || sp.faults ? 'warn' : 'ok',
					_('Speaker'),
					sp.error || (sp.faults ? _n(sp.faults, '{n} fault', '{n} faults') : stw(sp.state || 'ready'))
				);
			}
		}
		w &&
			!sys &&
			a(
				w.associated && w.signal_dbm >= -70 ? 'ok' : 'warn',
				_('Wi-Fi'),
				w.associated
					? (sig(w.signal_dbm) || _('Connected')) +
							(w.signal_dbm != null ? ' · ' + mi(w.signal_dbm) + ' dBm' : '') +
							(w.band ? ' · ' + w.band : '')
					: _('Not connected')
			);
		if (j) {
			const n = j.ntp || {},
				[c, d] = ntp(n);
			a(!n.server ? 'off' : n.synchronized ? 'ok' : 'warn', _('Clock'), n.synchronized ? d : c);
			a(
				j.ssh_password_sync === false ? 'warn' : 'ok',
				_('SSH password'),
				j.ssh_password_sync === false ? _('Not synchronized') : _('In sync')
			);
			j.state == 'ready' || a('warn', _('Platform'), j.version + ' · ' + j.state);
		}
		return L;
	}
	/* Status and Health: folded, and open by themselves on a problem; closed again when it clears. */
	function foldOn(d, n) {
		if (n && !d.dataset.a) {
			d.dataset.a = 1;
			d.open = true;
		} else if (!n && d.dataset.a) {
			delete d.dataset.a;
			d.open = false;
		}
	}
	const hlist = L =>
		L.map(([t, k, v]) =>
			h('li', null, h('span', { class: 'ico ' + t }, ico(t)), h('span', { text: k }), h('span', { text: v }))
		);
	function ov() {
		const L = hl(),
			n = L.filter(x => x[0] == 'warn' || x[0] == 'crit').length,
			w = S.q?.wifi;
		$('ov-stats').replaceChildren(...hlist(L));
		const LS = hl(1),
			ns = LS.filter(x => x[0] == 'warn' || x[0] == 'crit').length;
		$('sy-stats').replaceChildren(...hlist(LS));
		const calm = !fwMsg && !installing && !instLate && !sirOn() && !S.m?.platform?.privacy;
		$('sy-sum').replaceChildren(LS.length ? (ns ? pill('warn', _n(ns, '{n} thing to check', '{n} things to check')) : calm ? pill('ok', _('All normal')) : '') : '');
		foldOn($('sy-fold'), ns);
		foldOn($('ov-fold'), n);
		tx('sy-hc', S['m@'] ? _('Checked {t}', { t: hm(new Date(S['m@'])) }) : '');
		/* Offline: the last known status replaces the live list. */
		$('ov-st').hidden = !!off;
		$('ov-last').hidden = !off;
		tx('ov-la', lastOk ? hm(new Date(lastOk)) : _('Unknown'));
		tx('ov-lw', w?.associated && w.signal_dbm != null ? mi(w.signal_dbm) + ' dBm' : _('Unknown'));
		$('ov-sum').replaceChildren(
			off
				? pill('crit', _('Not answering'))
				: L.length
					? n
						? pill('warn', _n(n, '{n} thing to check', '{n} things to check'))
						: calm
							? pill('ok', _('All normal'))
							: ''
					: ''
		);
	}
	/* Stills: a JPEG of each lens, one request at a time and only as often as the page on
	   screen wants them, since each one takes the video encoder a moment. */
	const STL = { 0: {}, 2: {} },
		lensCh = () => 2 * lens;
	let stPaused = 0,
		stBusy = 0,
		stWas = 0,
		stT;
	const stMoving = () => fast > Date.now() || S.m?.platform?.moving,
		stWants = () =>
		(route == 'overview'
			? stPaused
				? []
				: [[0, 5e3], [2, 5e3]]
			: route == 'pantilt'
				? [[0, stMoving() ? 2e3 : 3e4], ...(S.a?.calibrated ? [[2, 3e4]] : [])]
				: route == 'image'
					? [[lensCh(), 3e4]]
					: []
		).filter(([ch]) => S.m?.streams?.some(x => x.channel == ch));
	function stShow() {
		const now = Date.now(),
			priv = S.m?.platform?.privacy;
		for (const e of $$('.still[data-ch]')) {
			if (e.closest('.page').hidden) continue;
			const im = e.dataset.ch == 'im',
				ch = im ? lensCh() : +e.dataset.ch,
				s = STL[ch],
				p = e.querySelector('.m'),
				at = e.querySelector('.at'),
				t = s.at ? hm(new Date(s.at)) : '',
				old = s.at && dur(Math.round((now - s.at) / 1e3));
			let i = e.querySelector('img');
			if (im) tx('im-pvl', ch ? _('Fixed lens') : _('Pan/tilt lens'));
			if (priv || !s.url) i?.remove();
			else {
				i ||= e.insertBefore(h('img', { alt: '' }), e.firstChild);
				if (i.getAttribute('src') != s.url) i.src = s.url;
				i.alt = ch ? _('Latest still of the fixed lens, from {t}', { t }) : _('Latest still of the pan/tilt lens, from {t}', { t });
			}
			p.className = 'm';
			if (priv) {
				p.textContent = _('Privacy mode is on: no stills.');
				at.textContent = '';
			} else if (off && s.url) {
				p.className = 'm old';
				p.replaceChildren(_('Last still, {old} old', { old }), h('span', { class: 'mono', text: t }));
				at.textContent = '';
			} else {
				p.textContent = s.url ? '' : s.err || (off ? _('No still yet.') : _('Getting a still…'));
				at.textContent = !t ? '' : e.dataset.at == 'ago' ? _('{t} · {d} ago', { t, d: old }) : t;
			}
		}
		$('ov-pz').disabled = !!(priv || off);
	}
	async function stGrab(ch) {
		stBusy = 1;
		const s = STL[ch],
			ac = new AbortController(),
			t = setTimeout(() => ac.abort(), 8e3);
		s.tried = Date.now();
		try {
			const r = await fetch(M + 'snapshot?channel=' + ch, { signal: ac.signal, cache: 'no-store' });
			if (r.ok) {
				const old = s.url;
				s.url = URL.createObjectURL(await r.blob());
				s.at = Date.now();
				s.err = '';
				old && setTimeout(() => URL.revokeObjectURL(old), 1e4);
			} else s.err = (await r.json().catch(() => null))?.error || _('No still right now.');
		} catch {
			s.err = _('No still right now.');
		}
		clearTimeout(t);
		stBusy = 0;
		stShow();
		stTick();
	}
	function stTick() {
		if (stBusy || off || !csrf || document.hidden || S.m?.platform?.privacy) return;
		/* the lens has just stopped: show where it ended up */
		if (stWas && !stMoving()) {
			STL[0].tried = 0;
			S['a@'] = 0;
		}
		stWas = stMoving();
		const now = Date.now(),
			w = stWants()
				.filter(([ch, every]) => now - (STL[ch].tried || 0) >= every)
				.sort((a, b) => (STL[a[0]].tried || 0) - (STL[b[0]].tried || 0));
		w[0] && stGrab(w[0][0]);
	}
	/* The picture settings reach the sensor within a second. */
	function stAfter() {
		clearTimeout(stT);
		stT = setTimeout(() => {
			STL[lensCh()].tried = 0;
			stTick();
		}, 1500);
	}
	$('ov-pz').onclick = e => {
		stPaused = !stPaused;
		e.currentTarget.textContent = stPaused ? _('Resume stills') : _('Pause stills');
		stTick();
	};
	/* Aiming from the fixed lens. The two lenses see about as much as each other, so the
	   dashed frame is most of the picture, and only the part inside it is drawn. */
	let ptView = 'live',
		aimKey = null,
		aimGo = null,
		aimErr = '';
	function aim() {
		return api(M + 'aim').then(d => {
			S.a = d;
			S['a@'] = Date.now();
			rAim();
		});
	}
	function rAim() {
		const a = S.a,
			pl = S.m?.platform,
			v = a?.view,
			fr = $('pt-fr'),
			tg = $('pt-tg');
		$('pt-view').hidden = !a?.calibrated;
		const aiming = !!a?.calibrated && ptView == 'aim';
		$('pt-aim').hidden = !aiming;
		$('pt-vl').hidden = aiming;
		if (!a?.calibrated) return;
		$('pt-aimb').disabled = !!(pl?.privacy || pl?.homing);
		const x0 = v ? Math.max(0, v.u - v.w / 2) : 1,
			x1 = v ? Math.min(1, v.u + v.w / 2) : 0,
			y0 = v ? Math.max(0, v.v - v.h / 2) : 1,
			y1 = v ? Math.min(1, v.v + v.h / 2) : 0,
			seen = x1 - x0 > 0.03 && y1 - y0 > 0.03;
		fr.hidden = !seen;
		if (seen) {
			fr.style.left = x0 * 100 + '%';
			fr.style.top = y0 * 100 + '%';
			fr.style.width = (x1 - x0) * 100 + '%';
			fr.style.height = (y1 - y0) * 100 + '%';
		}
		if (aimGo && !pl?.moving && Date.now() - aimGo.at > 1500) aimGo = null;
		const t = aimGo || aimKey;
		tg.hidden = !t;
		if (t) {
			tg.style.left = t.u * 100 + '%';
			tg.style.top = t.v * 100 + '%';
			tg.classList.toggle('l', t.u > 0.6);
			tg.firstChild.textContent = aimGo ? _('Turning here…') : '';
		}
		tx('pt-aimn', aimErr || (seen ? '' : _('The pan/tilt lens is looking outside this view.')));
	}
	async function aimAt(u, v) {
		aimGo = { u, v, at: Date.now() };
		aimKey = null;
		aimErr = '';
		fast = Date.now() + 4e3;
		rAim();
		try {
			await api(M + 'aim', { method: 'POST', json: { u, v } });
			media().catch(noop);
		} catch (e) {
			aimGo = null;
			aimErr = e.message;
			rAim();
		}
	}
	seg($('pt-view'), v => {
		ptView = v;
		pick($('pt-view'), v);
		rAim();
	});
	$('pt-aimb').onclick = e => {
		const r = e.currentTarget.getBoundingClientRect();
		/* a keyboard's click has no pointer: the first places the target, the next goes there */
		if (e.detail) aimAt((e.clientX - r.left) / r.width, (e.clientY - r.top) / r.height);
		else if (aimKey) aimAt(aimKey.u, aimKey.v);
		else {
			aimKey = { u: 0.5, v: 0.5 };
			rAim();
		}
	};
	$('pt-aimb').onkeydown = e => {
		const d = { ArrowLeft: [-1, 0], ArrowRight: [1, 0], ArrowUp: [0, -1], ArrowDown: [0, 1] }[e.key];
		if (!d) return;
		e.preventDefault();
		aimKey ||= { u: 0.5, v: 0.5 };
		aimKey.u = Math.min(1, Math.max(0, aimKey.u + d[0] * 0.04));
		aimKey.v = Math.min(1, Math.max(0, aimKey.v + d[1] * 0.04));
		rAim();
	};
	/* Camera controls (GET/POST /api/v1/controls): night mode, infrared, white light and
	   picture, kept by the camera across restarts. POST takes any subset and answers with
	   the full state; the engine applies it within a second, so the page reads it again. */
	/* Schedules run in the camera's own time zone (System, Time zone) when it has one
	   (live.zone "camera"); otherwise in the offset the browser sends with a schedule. */
	const camZone = () => S.x?.live.zone == 'camera',
		tz = () => (camZone() ? {} : { utc_offset_min: -new Date().getTimezoneOffset() }),
		utc = m => 'UTC' + (m < 0 ? '−' : '+') + p2((Math.abs(m) / 60) | 0) + ':' + p2(Math.abs(m) % 60),
		num = n => String(n).replace(/\B(?=(\d{3})+(?!\d))/g, ' '),
		mins = t => +t.slice(0, 2) * 60 + +t.slice(3),
		inWin = (m, a, b) => a != b && (a < b ? m >= a && m < b : m >= a || m < b);
	let xt = 0;
	async function putX(json) {
		const d = await api(M + 'controls', { method: 'POST', json });
		S.x = d;
		S['x@'] = Date.now();
		rControls(d);
		clearTimeout(xt);
		xt = setTimeout(() => controls().catch(noop), 1200);
		route == 'image' && stAfter();
		return d;
	}
	/* Seconds still to go on a live countdown, counted on between reads. */
	const left = s => Math.max(0, Math.round(s - age('x') / 1e3)),
		mmss = s => ((s / 60) | 0) + ':' + p2(s % 60);
	function nightLine() {
		const x = S.x;
		if (!x) return '';
		const n = x.night,
			L = x.live,
			now = L.night;
		if (now == 'unknown') return _('Starting…');
		const dark = now == 'night',
			since = L.since_s ? dur(L.since_s + age('x') / 1e3) : 0;
		let t = dark ? (since ? _('Night now, for the last {t}.', { t: since }) : _('Night now.')) : since ? _('Day now, for the last {t}.', { t: since }) : _('Day now.');
		if (L.privacy) t = _('Day mode during privacy, which its black-out needs. Then back to {m}.', { m: MODE[n.mode] || n.mode });
		else if (n.mode == 'auto') {
			const wait = left(n.wait_s - L.pending_s),
				hold = left(L.hold_left_s || 0);
			t +=
				' ' +
				(L.level == null
					? _('No darkness reading yet; it stays as it is.')
					: !L.pending_s
						? dark
							? _('Back to day when the level falls to {n}.', { n: num(n.day_ev) })
							: _('Night when the level reaches {n}.', { n: num(n.night_ev) })
						: hold > wait
							? dark
								? _('Switching to day in {t} if it stays light: at least 5 minutes pass between automatic switches, and longer if they keep reversing.', { t: dur(hold) })
								: _('Switching to night in {t} if it stays dark: at least 5 minutes pass between automatic switches, and longer if they keep reversing.', { t: dur(hold) })
							: wait
								? dark
									? _('Switching to day in {t} if it stays light.', { t: dur(wait) })
									: _('Switching to night in {t} if it stays dark.', { t: dur(wait) })
								: dark
									? _('Switching to day now if it stays light.')
									: _('Switching to night now if it stays dark.'));
		} else if (n.mode == 'schedule') t += ' ' + _('Night from {a} until {b}, camera time.', { a: n.from, b: n.until }) + camNow() + clockNote();
		else t += ' ' + _('It stays so until you change it.');
		return t + (L.error ? ' ' + _('The camera reports: {m}', { m: L.error }) : '');
	}
	function wlNote() {
		const x = S.x,
			p = S.m?.platform,
			on = p?.white_light >= 0 ? p.white_light == 1 : x?.live.white_light;
		if (!x) return p?.white_light < 0 ? _('Not set since the camera started') : on ? _('On') : _('Off');
		const s = x.white_light.schedule,
			tl = left(x.live.trigger_left_s);
		if (tl) return _('On for {t} more (triggered)', { t: mmss(tl) });
		return s.enabled
			? on
				? _('On · scheduled {a}–{b}', { a: s.from, b: s.until })
				: _('Off · scheduled {a}–{b}', { a: s.from, b: s.until })
			: on
				? _('On')
				: _('Off');
			}
	/* The schedule line: the zone the camera uses, and the next change. */
	function wlWhen() {
		const x = S.x,
			s = x.white_light.schedule,
			m = (x.live.local_minutes + age('x') / 6e4) % 1440,
			a = mins(s.from),
			b = mins(s.until),
			inside = inWin(m, a, b),
			e = inside ? b : a,
			today = e > m,
			dz = tz().utc_offset_min;
		return (
			(a == b
				? _('From and until are the same, so the light never turns on.')
				: (camZone() ? _('Every day, camera time ({z}, set under System).', { z: utc(x.utc_offset_min) }) : _('Every day, camera time ({z}).', { z: utc(x.utc_offset_min) })) +
					' ' +
					(inside
						? x.live.clock_waiting
							? _('Off until the clock is confirmed, up to 30 minutes after a start.')
							: today
								? _('On now, off today at {t}.', { t: s.until })
								: _('On now, off tomorrow at {t}.', { t: s.until })
						: today
							? _('Next: today {t}.', { t: s.from })
							: _('Next: tomorrow {t}.', { t: s.from }))) +
			(dz != null && x.utc_offset_min != dz ? ' ' + _('Saving uses this device’s zone, {z}.', { z: utc(dz) }) : '') +
			camNow() + clockNote()
		);
	}
	function lights() {
		const x = S.x,
			p = S.m?.platform;
		const mode = x?.night.mode ?? (p?.night >= 0 ? (p.night ? 'night' : 'day') : null);
		$$('.night').forEach(s => busy(s) || pick(s, mode));
		p && $$('.sw.wl').forEach(b => busy(b) || sw(b, p.white_light == 1));
		$$('.wl-note').forEach(e => (e.textContent = wlNote()));
		$$('.night-note').forEach(e => (e.textContent = x ? nightLine() : ''));
	}
	/* Darkness: the ISP's exposure value for the pan/tilt lens (higher is darker), with both levels. */
	function darkness() {
		const x = S.x,
			n = x.night,
			lv = x.live.level,
			top = Math.max(n.night_ev * 1.4, (lv || 0) * 1.1),
			at = v => Math.min(100, (v / top) * 100) + '%',
			[fill, d, nt, dl, nl] = $('im-dm').children;
		fill.style.width = lv == null ? '0' : at(lv);
		d.style.left = dl.style.left = at(n.day_ev);
		nt.style.left = nl.style.left = at(n.night_ev);
		tx('im-lv', lv == null ? _('No reading') : num(lv) + ' · ' + (MODE[x.live.night] || x.live.night));
		$('im-dm').setAttribute(
			'aria-label',
			lv == null ? _('Darkness meter: no reading, back to day below {d}, night above {n}', { d: num(n.day_ev), n: num(n.night_ev) }) : _('Darkness meter: now {v}, back to day below {d}, night above {n}', { v: num(lv), d: num(n.day_ev), n: num(n.night_ev) })
		);
	}
	const keep = i => document.activeElement == i || i.dataset.d;
	function setIn(i, v) {
		keep(i) || (i.value = v);
	}
	/* Durations in words for the selects and the trigger button: "10 seconds", "2 minutes". */
	const long = s => (s < 60 ? un(s, 'second', 'long') : s % 60 ? dur(s) : un(s / 60, 'minute', 'long'));
	/* A select of durations, with the current value added when it is not one of them. */
	function durs(el, list, v) {
		const L = list.includes(v) ? list : [...list, v].sort((a, b) => a - b);
		if (el.dataset.k != L + '') {
			el.dataset.k = L;
			el.replaceChildren(...L.map(s => h('option', { value: s }, long(s))));
		}
		busy(el) || (el.value = v);
	}
	/* The name on the picture: what its small font can draw. */
	const ovName = () => name().replace(/[^ -~]/g, '').slice(0, 16);
	function ovShow(o) {
		$('im-ovc').hidden = !o;
		document.body.classList.toggle('ov-tl', !!o?.enabled && o.position == 'top-left');
		if (!o) return;
		busy($('im-ovs')) || sw($('im-ovs'), o.enabled);
		$('im-ovf').hidden = $('im-ovb').hidden = !o.enabled;
		setIn($('im-ovp'), o.position);
		setIn($('im-ovm'), o.format);
		busy($('im-ovn')) || ($('im-ovn').checked = !!o.name);
		tx('im-ovt', _('Add the camera name ({a})', { a: o.name || ovName() }));
		tx('im-ovw', o.enabled && S.x?.live?.clock_ok === false ? _('Not on the picture yet: it waits until network time or you have set the clock, because the time a restart restores can be hours behind.') : '');
	}
	function rControls(x) {
		privacyShow();
		patrolShow();
		ovShow(x.overlay);
		sirShow(x.siren);
		attn();
		const n = x.night,
			w = x.white_light,
			wm = w.schedule.enabled ? 'schedule' : w.on ? 'on' : 'off';
		lights();
		$('im-picn').hidden = x.live.night != 'night';
		$('im-auto').hidden = n.mode != 'auto';
		$('im-sch').hidden = n.mode != 'schedule';
		n.mode == 'auto' && darkness();
		setIn($('im-ne'), num(n.night_ev).replace(/ /g, ' '));
		setIn($('im-de'), num(n.day_ev).replace(/ /g, ' '));
		setIn($('im-nf'), n.from);
		setIn($('im-nu'), n.until);
		evWarn();
		durs($('im-wait'), [10, 30, 60, 120, 300, 600], n.wait_s);
		busy($('im-ir')) || pick($('im-ir'), n.ir);
		busy($('im-wlm')) || pick($('im-wlm'), wm);
		$('im-wls').hidden = wm != 'schedule';
		setIn($('im-wf'), w.schedule.from);
		setIn($('im-wu'), w.schedule.until);
		durs($('im-wd'), [30, 60, 120, 300, 600, 1800], w.trigger_s);
		tx('im-wt', _('Turn on for {t}', { t: long(w.trigger_s) }));
		rCount();
		busy($('im-af')) || pick($('im-af'), x.anti_flicker);
		const pic = x.picture[lens];
		pic && PIC.forEach(op => setSl('im-' + op, pic[op]));
		S.c && ha(S.c);
		syncZone(x);
		S.y && rRestart(S.y);
	}
	function rCount() {
		const x = S.x,
			w = x.white_light,
			tl = left(x.live.trigger_left_s);
		tx('im-wln', w.schedule.enabled ? wlWhen() : '');
		tx(
			'im-wtn',
			tl
				? w.schedule.enabled
					? _('On for {t} more, then back to its schedule.', { t: mmss(tl) })
					: w.on
						? _('On for {t} more, then it stays on.', { t: mmss(tl) })
						: _('On for {t} more, then off.', { t: mmss(tl) })
						: ''
		);
	}
	/* Day and night (Overview and Image): the mode; schedules follow this device's zone. */
	const NT = {
		day: _('Day mode on'),
		night: _('Night vision on'),
		auto: _('Day and night switch by themselves'),
		schedule: _('Night on a schedule')
	};
	$$('.night').forEach(s =>
		seg(s, v =>
			run(s, async () => {
				await putX({ night: { mode: v }, ...(v == 'schedule' ? tz() : {}) });
				pick(s, v); // lights() leaves busy controls alone
				toast(NT[v]);
			})
		)
	);
	/* Darkness levels: whole numbers, spaces allowed ("75 000"); day must stay below night. */
	const lvl = v => ((v = v.replace(/[\s,.'_]/g, '')), /^\d{1,8}$/.test(v) && v >= 1 && v <= 1e7 ? +v : null),
		evIn = [$('im-ne'), $('im-de')];
	/* A soft warning, not a refusal: with the filter open the level reads about 3.6x lower at
	   twilight, so a higher day level reads dark again right after the switch and it flips back. */
	function evWarn() {
		const [ne, de] = evIn.map(e => lvl(e.value)),
			high = ne != null && de != null && de < ne && de > ne / 3.6;
		$('im-evw').hidden = !high;
		foldOn($('im-lvf'), high);
		high &&
			tx(
				'im-evt',
				_('A day level of {a} may make it flip back and forth at dusk and dawn. For a night level of {b}, try {c} or lower.', { a: num(de), b: num(ne), c: num(Math.floor(ne / 3.6)) })
			);
	}
	evIn.forEach(i => {
		i.addEventListener('input', () => {
			i.dataset.d = 1;
			evWarn();
		});
		i.onchange = () => {
			const [ne, de] = evIn.map(e => lvl(e.value));
			evIn.forEach(e => {
				e.removeAttribute('aria-invalid');
				clr(e);
			});
			for (const [e, v] of [
				[evIn[0], ne],
				[evIn[1], de]
			])
				if (v == null) return bad(e, _('Use a whole number from 1 to 10 000 000.'));
			if (de >= ne) return bad(i, _('The day level must be below the night level.'));
			run(
				i,
				async () => {
					await putX({ night: { night_ev: ne, day_ev: de } });
					evIn.forEach(e => delete e.dataset.d);
					toast(_('Levels saved'));
				},
				i
			);
		};
	});
	$('im-wait').onchange = e =>
		run(e.target, async () => {
			await putX({ night: { wait_s: +e.target.value } });
			toast(_('Saved'));
		});
	/* Schedules: a pair of "HH:MM" times; saving also sends this device's zone. */
	function times(a, b, key) {
		[a, b].forEach(i => {
			i.addEventListener('input', () => (i.dataset.d = 1));
			i.onchange = () => {
				if (!a.value || !b.value) return bad(i, _('Enter both times.'));
				if (a.value == b.value) return bad(i, _('From and until must differ.'));
				run(
					i,
					async () => {
						const t = { from: a.value, until: b.value };
						await putX({ ...(key ? { white_light: { schedule: t } } : { night: t }), ...tz() });
						[a, b].forEach(e => delete e.dataset.d);
						toast(_('Schedule saved'));
					},
					i
				);
			};
		});
	}
	times($('im-nf'), $('im-nu'));
	times($('im-wf'), $('im-wu'), 1);
	seg($('im-ir'), v =>
		run($('im-ir'), async () => {
			await putX({ night: { ir: v } });
			pick($('im-ir'), v);
			toast(v == 'off' ? _('Infrared LEDs stay off') : _('Infrared LEDs on at night'));
		})
	);
	/* White light: Off, On, or a schedule; the Overview switch changes it until the next schedule edge. */
	seg($('im-wlm'), v =>
		run($('im-wlm'), async () => {
			const s = S.x?.white_light.schedule || {};
			await putX(
				v == 'schedule'
					? { white_light: { schedule: { enabled: true, from: s.from, until: s.until } }, ...tz() }
					: { white_light: { on: v == 'on', schedule: { enabled: false } } }
			);
			pick($('im-wlm'), v);
			toast(v == 'schedule' ? _('White light on a schedule') : v == 'on' ? _('White light on') : _('White light off'));
		})
	);
	$('im-wd').onchange = e =>
		run(e.target, async () => {
			await putX({ white_light: { trigger_s: +e.target.value } });
			toast(_('Saved'));
		});
	$('im-wt').onclick = act(async () => {
		await ctl('white_light_trigger', 0); // 0: for trigger_s
		await controls();
		media().catch(noop);
		toast(_('White light on for {t}', { t: long(S.x.white_light.trigger_s) }));
	});
	$$('.sw.wl').forEach(
		b =>
			(b.onclick = () => {
				const on = b.getAttribute('aria-checked') != 'true';
				run(b, async () => {
					await ctl('white_light', +on);
					S.m && (S.m.platform.white_light = +on);
					sw(b, on);
					lights();
					toast(on ? _('White light on') : _('White light off'));
					controls().catch(noop);
				});
			})
	);
	/* Picture per lens, from the camera's remembered values. */
	const PICN = { brightness: _('Brightness'), contrast: _('Contrast'), saturation: _('Saturation'), sharpness: _('Sharpness'), denoise: _('Noise reduction') },
		PIC = ['brightness', 'contrast', 'saturation', 'sharpness', 'denoise'],
		picOf = (l, p) => ({ picture: l ? [null, p] : [p] });
	PIC.forEach(op =>
		$('im-sl').append(
			slider('im-' + op, PICN[op], 0, 255, 128, String, v =>
				putX(picOf(lens, { [op]: v }))
			)
		)
	);
	seg($('im-lens'), v => {
		lens = +v;
		pick($('im-lens'), v);
		PIC.forEach(op => setSl('im-' + op, S.x?.picture[lens][op] ?? 128, 1));
		stShow();
		stTick();
	});
	const putO = (b, overlay) => run(b, () => putX({ overlay }), $('im-ovc')).then(() => ovShow(S.x?.overlay));
	$('im-ovs').onclick = e => putO(e.currentTarget, { enabled: e.currentTarget.getAttribute('aria-checked') != 'true' });
	$('im-ovp').onchange = e => putO(e.target, { position: e.target.value });
	$('im-ovm').onchange = e => putO(e.target, { format: e.target.value });
	$('im-ovn').onchange = e => putO(e.target, { name: e.target.checked ? ovName() : '' });
	$('im-reset').onclick = act(async () => {
		const p = Object.fromEntries(PIC.map(op => [op, 128]));
		await putX(picOf(lens, p));
		PIC.forEach(op => setSl('im-' + op, 128, 1));
		toast(_('Picture reset to neutral'));
	});
	seg($('im-af'), v =>
		run($('im-af'), async () => {
			await putX({ anti_flicker: v });
			pick($('im-af'), v);
			toast(v == 'off' ? _('Anti-flicker off') : _('Anti-flicker for {v} Hz mains', { v }));
		})
	);
	const rm = f => [(f >> 1) & 1, (f ^ (f >> 1)) & 1];
	function orient() {
		const c = S.c;
		if (!c) return;
		$('im-or').classList.toggle('one', c.sensors < 2);
		$$('#im-or .sw').forEach(b => {
			if (busy(b)) return;
			const [R, M] = rm(c['flip' + b.dataset.s] | 0);
			sw(b, b.dataset.k == 'r' ? R : M);
		});
	}
	$('im-or').onclick = e => {
		const b = e.target.closest('.sw');
		if (!b || !S.c) return;
		const s = +b.dataset.s;
		let [R, M] = rm(S.c['flip' + s] | 0);
		b.dataset.k == 'r' ? (R ^= 1) : (M ^= 1);
		const f = (R ? 3 : 0) ^ M;
		run(b, async () => {
			await ctl('flip', f, s);
			S.c['flip' + s] = f;
			sw(b, b.dataset.k == 'r' ? R : M);
			orient();
			toast(_('Saved. Video restarts for a few seconds.'));
			setTimeout(() => config().catch(noop), 5e3);
		});
	};
	/* ptz: a velocity with a 1.2 s lease, renewed while held; ptz_stop on release, in order.
	   Speed scales the unit direction: Slow 30, Normal 60, Fast 100 (of 100). */
	const SPEEDS = [30, 60, 100];
	let speed = SPEEDS.includes(ls.get(SPEED)) ? ls.get(SPEED) : 60;
	seg($('pt-speed'), v => {
		speed = +v;
		ls.set(SPEED, speed);
		pick($('pt-speed'), v);
	});
	function send(op, x, y) {
		pn++;
		pq = pq
			.then(() => ctl(op, x, y))
			.catch(e => {
				if (op != 'ptz') return;
				ptzEnd(1);
				e.kind != 'auth' && err($('pt-stop'), e.kind == 'off' ? OFF : e.message);
			})
			.finally(() => pn--);
	}
	function ptzGo(b) {
		if (pb == b || !csrf || off) return;
		pb && pb.classList.remove('on');
		clr($('pt-stop'));
		pb = b;
		b.classList.add('on');
		const [x, y] = b.dataset.d.split(',').map(n => n * speed);
		send('ptz', x, y);
		clearInterval(pt);
		pt = setInterval(() => pn || send('ptz', x, y), 400);
		fast = Date.now() + 4e3;
	}
	function ptzEnd(q) {
		if (!pb) return;
		clearInterval(pt);
		pb.classList.remove('on');
		pb = null;
		fast = Date.now() + 3e3;
		q !== 1 && send('ptz_stop');
	}
	function hold(b, dn, up) {
		b.addEventListener('pointerdown', e => {
			if (e.button) return;
			e.preventDefault();
			b.setPointerCapture(e.pointerId);
			dn();
		});
		['pointerup', 'pointercancel', 'lostpointercapture'].forEach(t => b.addEventListener(t, up));
		const k = e => e.key == ' ' || e.key == 'Enter';
		b.onkeydown = e => {
			if (k(e)) {
				e.preventDefault();
				e.repeat || dn();
			}
		};
		b.onkeyup = e => {
			if (k(e)) {
				e.preventDefault();
				up();
			}
		};
		b.oncontextmenu = e => e.preventDefault();
	}
	$$('#pt-pad [data-d]').forEach(b => hold(b, () => ptzGo(b), ptzEnd));
	$('pt-stop').onclick = () => {
		ptzEnd(1);
		send('ptz_stop');
	};
	const AR = { ArrowUp: 'u', ArrowDown: 'd', ArrowLeft: 'l', ArrowRight: 'r' };
	$('pt-move').onkeydown = e => {
		const s = AR[e.key];
		if (!s || e.target.closest('.seg')) return;
		e.preventDefault();
		e.repeat || ptzGo($('pt-pad').querySelector('.' + s));
	};
	$('pt-move').onkeyup = e => AR[e.key] && !e.target.closest('.seg') && ptzEnd();
	addEventListener('blur', ptzEnd);
	const where = p =>
		p.position_known ? _('Pan {a} · Tilt {b}', { a: sg(p.position_x), b: sg(p.position_y) }) : _('Position unknown');
	function pos() {
		const p = S.m.platform;
		if (!p) {
			$$('button', $('pt-pad')).forEach(b => (b.disabled = true));
			return tx('pt-pos', _('Pan/tilt is unavailable: its saved presets could not be read.'));
		}
		const t = where(p),
			l = t + ' · ' + (p.privacy ? _('privacy mode') : p.homing ? _('calibrating') : p.moving ? _('moving') : _('stopped'));
		$$('.pos').forEach(e => (e.textContent = t));
		$('pt-pos').textContent != l && tx('pt-pos', l);
		$$('button', $('pt-pad')).forEach(b => (b.disabled = !!p.privacy));
		$('pt-rc').disabled = !!(p.privacy || p.homing);
		tx(
			'pt-rcn',
			p.homing ? _('Running now.') : p.homed_ago_s != null ? _('Last done {t} ago.', { t: dur(p.homed_ago_s) }) : _('Last done at boot.')
		);
		map();
	}
	/* Privacy mode and the patrol live in the saved controls; on a schedule, the switch holds until its next edge. */
	const STOP_DWELL = [5, 10, 15, 30, 60, 120, 300];
	function patrolShow() {
		const x = S.x;
		if (!x?.patrol) return;
		const T = x.patrol,
			L = P(),
			by = new Map(L.map(p => [p.slot, p])),
			stops = T.stops,
			live = x.live.patrol || {},
			nm = s => by.get(s)?.name || _('a deleted preset'),
			k = JSON.stringify([stops, L]);
		if ($('pt-stops').dataset.k != k) {
			$('pt-stops').dataset.k = k;
			$('pt-stops').replaceChildren(
				...stops.map((s, i) => {
					const d = h('select', { 'aria-label': _('Time at {a}', { a: nm(s.slot) }) });
					durs(d, STOP_DWELL, s.dwell_s);
					d.onchange = () => stopsPut(stops.map((o, j) => (j == i ? { ...o, dwell_s: +d.value } : o)), d);
					return h(
						'li',
						null,
						h('i', { text: i + 1 }),
						h('b', { text: nm(s.slot) }),
						d,
						h('button', { class: 'btn', 'aria-label': _('Remove {c}', { c: nm(s.slot) }), onclick: e => stopsPut(stops.filter((o, j) => j != i), e.currentTarget) }, _('Remove'))
					);
				})
			);
			const a = $('pt-add'),
				was = a.value;
			a.replaceChildren(...L.map(p => h('option', { value: p.slot }, `${p.slot} · ${p.name}`)));
			was && L.some(p => p.slot == was) && (a.value = was);
		}
		$('pt-addb').disabled = !L.length || stops.length >= 16;
		busy($('pt-pts')) || sw($('pt-pts'), T.enabled);
		$('pt-pts').disabled = !stops.length;
		durs($('pt-pz'), [0, 60, 120, 300, 600, 1800], T.pause_s);
		$('pt-pz').options[0]?.value == '0' && ($('pt-pz').options[0].textContent = _('Do not wait'));
		/* a move takes about 450 steps a second; the status's scale is 200 units end to end */
		let secs = 0;
		stops.forEach((s, i) => {
			const a = by.get(s.slot),
				b = by.get(stops[(i + 1) % stops.length].slot);
			secs += s.dwell_s + (a && b ? ((Math.abs(a.x - b.x) * 20.4 + Math.abs(a.y - b.y) * 6.2) / 450) | 0 : 0);
		});
		tx('pt-ptn', stops.length > 1 ? _('One round takes about {t}.', { t: dur(secs) }) : '');
		const left = Math.max(0, Math.round(live.left_s - age('x') / 1e3));
		tx(
			'pt-ptst',
			!T.enabled
				? stops.length
					? ''
					: _('Add a preset or two to patrol between.')
				: {
						moving: _('Going to {a}.', { a: nm(live.slot) }),
						dwell: _('At {a}, for {t} more.', { a: nm(live.slot), t: dur(left) }),
						paused: _('Paused because someone steered; carries on in {t}.', { t: dur(left) }),
						privacy: _('Waiting: privacy mode is on.'),
						homing: _('Waiting for the calibration to end.'),
						no_stops: _('None of these presets exist any more.')
					}[live.state] || ''
		);
	}
	function stopsPut(stops, at) {
		return run(at, async () => {
			await putX({ patrol: { stops, ...(stops.length ? {} : { enabled: false }) } });
			patrolShow();
		});
	}
	/* Schedules run on the camera's clock, which a power cut can leave hours behind. */
	const camNow = () => {
			const m = S.x?.live?.local_minutes;
			return m == null ? '' : ' ' + _('Camera time now {t}.', { t: p2(Math.floor(m / 60)) + ':' + p2(m % 60) });
		},
		clockNote = () =>
			S.x?.live?.clock_ok === false
				? ' ' + _('Nothing has confirmed this clock since the camera started, so this can be off by as long as it had no power.') + ' ' +
					(S.x.live.network_time ? _('Network time has not answered yet; you can also set the clock under System.') : _('Turn on network time, or set the clock under System.'))
				: S.x?.live?.network_time === false
					? ' ' + _('Network time is off, so after a power cut this runs late by the time without power until the clock is set again.')
					: '';
					function privacyShow() {
		const x = S.x;
		if (!x?.privacy) return;
		const c = x.privacy.schedule;
		$$('.sw.pv').forEach(b => busy(b) || sw(b, x.live.privacy));
		busy($('pt-pls')) || sw($('pt-pls'), c.enabled);
		$('pt-plf').hidden = !c.enabled;
		setIn($('pt-pf'), c.from);
		setIn($('pt-pu'), c.until);
		const ps = $('pt-pvs'),
			L = P(),
			key = L.map(p => p.slot + p.name) + '';
		if (ps.dataset.k != key) {
			ps.dataset.k = key;
			ps.replaceChildren(h('option', { value: 0 }, _('Looking down')), ...L.map(p => h('option', { value: p.slot }, `${p.slot} · ${p.name}`)));
		}
		busy(ps) || (ps.value = x.privacy.slot);
		tx(
			'pt-pln',
			!c.enabled
				? ''
				: (x.live.zone == 'camera' ? _('Every day from {a} to {b}, in the camera’s time zone.', { a: c.from, b: c.until }) : _('Every day from {a} to {b}, in this device’s time zone.', { a: c.from, b: c.until })) +
					' ' +
					_('The switch above lasts until the next change.') +
					camNow() +
					clockNote() +
					(x.live.clock_waiting && x.live.privacy ? ' ' + _('Privacy stays on until the clock is confirmed, for up to 30 minutes after a start.') : '')
				);
	}
	$('pt-pvs').onchange = e => {
		const el = e.target;
		run(el, async () => {
			await putX({ privacy: { slot: +el.value } });
			toast(_('Saved: this applies the next time privacy mode turns on'));
		}, $('pt-pln'));
	};
	const pvOff = b =>
		run(b, async () => {
			await putX({ privacy: { on: false } });
			$$('.sw.pv').forEach(s => sw(s, false));
			toast(_('Privacy mode off: video is back'));
			media().catch(noop);
		});
	$('at-privb').onclick = e => pvOff(e.currentTarget);
	$$('.sw.pv').forEach(b => (b.onclick = () => {
		if (b.getAttribute('aria-checked') == 'true') return pvOff(b);
		sheet({
			title: _('Turn on privacy mode?'),
			text: _('Video from both lenses goes black and the microphone is muted until you turn it off. Your NVR keeps recording the black picture. The pan/tilt lens points away and comes back afterwards.'),
			ok: _('Turn on'),
			icon: 'pantilt',
			run: async () => {
				await putX({ privacy: { on: true } });
				$$('.sw.pv').forEach(s => sw(s, true));
				toast(_('Privacy mode on'));
				media().catch(noop);
			}
		});
	}));
	$('pt-pls').onclick = e => {
		const b = e.currentTarget,
			on = b.getAttribute('aria-checked') != 'true';
		run(b, async () => {
			await putX({ privacy: { schedule: { enabled: on, from: $('pt-pf').value || '22:00', until: $('pt-pu').value || '06:00' } }, ...tz() });
			sw(b, on);
			toast(on ? _('Privacy schedule on') : _('Privacy schedule off'));
		});
	};
	['pt-pf', 'pt-pu'].forEach(i => {
		$(i).addEventListener('input', e => (e.target.dataset.d = 1));
		$(i).onchange = e => {
			const el = e.target;
			if (!el.value) return;
			run(
				el,
				async () => {
					await putX({ privacy: { schedule: { from: $('pt-pf').value, until: $('pt-pu').value } }, ...tz() });
					delete el.dataset.d;
					toast(_('Privacy schedule saved'));
				},
				el.closest('.g2')
			);
		};
	});
	$('pt-rc').onclick = () =>
		sheet({
			title: _('Recalibrate pan and tilt?'),
			text: _('Both motors run to their end stops and the lens comes back to where it is now, about 30 seconds. Video keeps running, and your presets stay where they are.'),
			ok: _('Recalibrate'),
			icon: 'pantilt',
			run: async () => {
				await ctl('ptz_recalibrate', 0, 0);
				toast(_('Recalibrating'));
				media().catch(noop);
			}
		});
	$('pt-pts').onclick = e => {
		const b = e.currentTarget,
			on = b.getAttribute('aria-checked') != 'true';
		run(b, async () => {
			await putX({ patrol: { enabled: on } });
			sw(b, on);
			toast(on ? _('Patrol on') : _('Patrol off'));
		});
	};
	$('pt-addb').onclick = e => {
		const slot = +$('pt-add').value;
		slot && stopsPut([...(S.x?.patrol.stops || []), { slot, dwell_s: 10 }], e.currentTarget);
	};
	$('pt-pz').onchange = e => {
		const b = e.target;
		run(b, () => putX({ patrol: { pause_s: +b.value } }), $('pt-pat'));
	};
	const P = () => S.m?.platform?.presets || [];
	/* Travel map: pan and tilt, presets included, share the -100..100 scale; tilt up is up. */
	let lastM = '';
	function map() {
		const p = S.m?.platform;
		if (!p) return;
		const L = P(),
			k = JSON.stringify([L, p.position_known, p.position_x, p.position_y]);
		if (k == lastM) return;
		lastM = k;
		const at = (e, x, y) => {
			const c = v => Math.max(-100, Math.min(100, +v || 0));
			e.style.left = (c(x) + 100) / 2 + '%';
			e.style.top = (100 - c(y)) / 2 + '%';
			return e;
		};
		$('pt-map').setAttribute(
			'aria-label',
			p.position_known
				? _n(L.length, 'Map of the travel range with the current aim and {n} preset position', 'Map of the travel range with the current aim and {n} preset positions')
				: _n(L.length, 'Map of the travel range with {n} preset position', 'Map of the travel range with {n} preset positions')
			);
		$('pt-map').firstElementChild.replaceChildren(
			...L.map(q => at(h('b', { title: q.name, text: q.slot }), q.x, q.y)),
			...(p.position_known ? [at(h('i', { class: 'me' }), p.position_x, p.position_y)] : [])
		);
	}
	/* Desktop shows all 16 slots (free ones offer "Save here"); a phone lists the used ones. */
	function presets() {
		const L = P(),
			k = JSON.stringify(L),
			noPlat = !!S.m && !S.m.platform;
		/* Before the check below: a repaired catalog can be the same empty list as before. */
		$('pt-empty').hidden = !!L.length || noPlat;
		$('pt-save').disabled = noPlat;
		if (k == lastP) return;
		lastP = k;
		closeMenu();
		const used = p =>
				h(
					'li',
					null,
					h('div', null, h('i', { text: p2(p.slot) }), h('b', { text: p.name })),
					h(
						'div',
						null,
						h(
							'button',
							{
								class: 'btn',
								'aria-label': _('Go to {n}', { n: p.name }),
								onclick: act(async () => {
									await ctl('preset_goto', p.slot);
									fast = Date.now() + 2e4;
									toast(_('Moving to “{name}”', { name: p.name }));
									media().catch(noop);
								})
							},
							_('Go')
						),
						h(
							'button',
							{
								class: 'btn',
								'aria-label': _('More actions for {n}', { n: p.name }),
								'aria-haspopup': 'menu',
								'aria-expanded': 'false',
								onclick: e => menu(p, e.currentTarget)
							},
							ico('more')
						)
					)
				);
		$('pt-list').replaceChildren(...L.map(used));
		$('pt-list').hidden = !L.length;
		tx('pt-count', L.length ? (L.length < 16 ? _('{a} of 16 used', { a: L.length }) : _('All 16 slots are in use. Delete a preset to save a new one.')) : '');
		map();
	}
	/* Preset names: 1-64 UTF-8 bytes, no control characters, unique ignoring ASCII case (as the camera checks). */
	const low = s => s.replace(/[A-Z]+/g, x => x.toLowerCase());
	function chk(v, slot) {
		v = v.trim();
		if (!v) no(_('Enter a name.'));
		if (new TextEncoder().encode(v).length > 64) no(_('That name is too long for the camera. Use a shorter one.'));
		if (/[\x00-\x1f\x7f]/.test(v)) no(_('Names cannot contain control characters.'));
		if (P().some(p => p.slot != slot && low(p.name) == low(v)))
			no(_('Another preset is already called “{v}”.', { v }));
		return v;
	}
	const nameIn = v => h('input', { id: 'sh-n', value: v, maxlength: 64, autocomplete: 'off' });
	async function saved(m) {
		toast(m);
		lastP = '';
		await media();
	}
	function save(slot) {
		const u = P().map(p => p.slot),
			open = [...Array(16).keys()].map(i => i + 1).filter(s => !u.includes(s));
		if (!open.length) return err($('pt-save'), _('All 16 slots are in use. Delete one first.'));
		slot = open.includes(slot) ? slot : open[0];
		const i = nameIn(_('Preset {n}', { n: slot })),
			sel = h(
				'select',
				{ id: 'sh-s' },
				open.map(s => h('option', { value: s, selected: s == slot }, _('Slot {n}', { n: p2(s) })))
			),
			p = S.m?.platform;
		let was = sel.value;
		sel.onchange = () => {
			if (i.value == _('Preset {n}', { n: was })) i.value = _('Preset {n}', { n: sel.value });
			was = sel.value;
		};
		sheet({
			title: _('Save this position'),
			text: p?.position_known ? _('Saves where the pan/tilt lens points now ({w}).', { w: where(p) }) : _('Saves where the pan/tilt lens points now.'),
			icon: 'pantilt',
			body: [fld(_('Name'), i), fld(_('Slot'), sel)],
			ok: _('Save preset'),
			focus: i,
			run: async () => {
				const s = +sel.value,
					n = chk(i.value, s);
				await media().catch(noop);
				if (P().some(q => q.slot == s)) no(_('Slot {s} has just been taken. Choose another.', { s }));
				await ctl('preset_set', s, 0, n);
				await saved(_('Saved “{n}” in slot {s}', { n, s }));
			}
		});
	}
	function rename(p) {
		const i = nameIn(p.name);
		sheet({
			title: _('Rename “{name}”', { name: p.name }),
			text: _('Slot {slot} keeps its position; your NVR shows the new name too.', { slot: p.slot }),
			icon: 'pantilt',
			body: [fld(_('Name'), i)],
			ok: _('Rename'),
			focus: i,
			run: async () => {
				const n = chk(i.value, p.slot);
				n != p.name && (await ctl('preset_rename', p.slot, 0, n));
				await saved(_('Renamed to “{n}”', { n }));
			}
		});
	}
	function replace(p) {
		const q = S.m?.platform;
		sheet({
			title: _('Replace “{name}”?', { name: p.name }),
			text: q?.position_known ? _('Slot {n} then points where the lens points now ({w}). Your NVR uses the new position too.', { n: p.slot, w: where(q) }) : _('Slot {n} then points where the lens points now. Your NVR uses the new position too.', { n: p.slot }),
			icon: 'pantilt',
			ok: _('Replace'),
			run: async () => {
				await ctl('preset_set', p.slot, 0, p.name);
				await saved(_('“{name}” now points here', { name: p.name }));
			}
		});
	}
	function remove(p) {
		sheet({
			title: _('Delete “{name}”?', { name: p.name }),
			text: _('Slot {slot} becomes free. Your NVR loses this preset too.', { slot: p.slot }),
			ok: _('Delete'),
			danger: 1,
			icon: 'pantilt',
			run: async () => {
				await ctl('preset_remove', p.slot);
				await saved(_('Preset deleted'));
			}
		});
	}
	/* The "…" menu: a small popup under the button; arrows move, Escape closes. */
	let pop = null;
	function closeMenu(focus) {
		if (!pop) return;
		const b = pop.btn;
		pop.remove();
		pop = null;
		b.setAttribute('aria-expanded', 'false');
		focus && b.focus();
	}
	function menu(p, b) {
		const again = pop?.btn == b;
		closeMenu();
		if (again) return;
		const item = (t, f, c) =>
			h(
				'button',
				{
					type: 'button',
					role: 'menuitem',
					class: c,
					tabindex: -1,
					onclick: () => {
						closeMenu(1);
						f(p);
					}
				},
				t
			);
		pop = h(
			'div',
			{ class: 'pop', role: 'menu', 'aria-label': _('Actions for {n}', { n: p.name }) },
			item(_('Rename…'), rename),
			item(_('Replace with current position'), replace),
			item(_('Delete…'), remove, 'dz')
		);
		pop.btn = b;
		pop.onkeydown = e => {
			const L = $$('button', pop),
				i = L.indexOf(document.activeElement),
				d = { ArrowDown: 1, ArrowUp: -1 }[e.key];
			if (e.key == 'Escape') {
				e.preventDefault();
				closeMenu(1);
			} else if (d) {
				e.preventDefault();
				L[(i + d + L.length) % L.length].focus();
			} else if (e.key == 'Tab') closeMenu();
		};
		b.closest('li').append(pop);
		b.setAttribute('aria-expanded', 'true');
		pop.firstChild.focus();
	}
	document.addEventListener(
		'pointerdown',
		e => pop && !pop.contains(e.target) && !pop.btn.contains(e.target) && closeMenu()
	);
	$('pt-save').onclick = () => save();
	const vf = v => v + ' · ' + (v == 60 ? _('unity') : (v > 60 ? '+' : '−') + Math.abs(v - 60) / 2 + ' dB'),
		aon = () => (S.c ? S.c.audio : S.m?.audio?.enabled);
	$('au-msl').append(
		slider('au-mv', _('Volume'), -30, 120, 80, vf, v => ctl('volume', v, 0)),
		slider('au-mg', _('Analog gain'), 0, 31, 22, String, v => ctl('gain', v, 0))
	);
	$('au-ssl').append(
		slider('au-sv', _('Volume'), -30, 120, 60, vf, v => ctl('volume', v, 1)),
		slider('au-sg', _('Analog gain (from the next talk)'), 0, 31, 25, String, v => ctl('gain', v, 1))
	);
	/* Level meters: n of N bars for -60..0 dBFS. */
	const bars = n =>
		h(
			'div',
			{ class: 'bars', role: 'meter', 'aria-valuemin': 0, 'aria-valuemax': n },
			[...Array(n)].map(() => h('i'))
		);
	function level(el, db) {
		const N = el.children.length,
			n = Math.max(0, Math.min(N, Math.round(((db + 60) / 60) * N)));
		if (el.n === n) return;
		el.n = n;
		[...el.children].forEach((b, i) => b.classList.toggle('on', i < n));
		el.setAttribute('aria-valuenow', n);
		el.setAttribute('aria-valuetext', db > -90 ? Math.round(db) + ' dBFS' : _('Silence'));
	}
	const mb = bars(12);
	mb.setAttribute('aria-label', _('Microphone level'));
	$('au-lv').prepend(h('span', { class: 'lbl', text: _('Sound level now') }), mb);
	function audio() {
		const A = S.m?.audio || {},
			mc = A.microphone || {},
			sp = A.speaker || {},
			en = aon(),
			k = !!(S.m || S.c),
			mu = A.muted,
			lv = A.level;
		$('au-body').hidden = k && !en;
		$('au-off').hidden = !k || !!en;
		$('au-sw').hidden = !k;
		busy($('au-on')) || sw($('au-on'), !!en);
		setSl('au-mv', mc.volume);
		setSl('au-mg', mc.gain);
		setSl('au-sv', sp.volume);
		setSl('au-sg', mc.speaker_gain);
		/* Pills only when something needs attention; the switch shows on/off. */
		$('au-mst').replaceChildren(
			mc.error || (mc.state && mc.state != 'running') ? pill('warn', mc.error ? _('Error') : stw(mc.state)) : ''
		);
		$('au-sst').replaceChildren(
			sp.error || sp.faults
				? pill('warn', sp.error ? _('Error') : _n(sp.faults, '{n} fault', '{n} faults'))
				: sp.state == 'talking'
					? pill('wait', _('Talking'))
					: ''
		);
		/* Older media releases report neither the mute state nor the level. */
		$('au-mic').hidden = typeof mu != 'boolean';
		busy($('au-mic')) || sw($('au-mic'), !mu);
		const pv = !!S.m?.platform?.privacy;
		$('au-mic').disabled = pv;
		$('au-lv').hidden = !lv;
		if (lv) {
			level(mb, mu ? -96 : lv.rms_dbfs);
			tx(
				'au-lvn',
				pv
					? _('Privacy mode has the microphone off until it ends.')
				: mu
					? _('The microphone is off: your NVR receives silence until you turn it back on.')
					: ''
				);
		}
		talkUI();
	}
	$('au-mic').onclick = e => {
		const b = e.currentTarget,
			on = b.getAttribute('aria-checked') != 'true';
		run(b, async () => {
			await ctl('mic_mute', on ? 0 : 1);
			S.m?.audio && (S.m.audio.muted = !on);
			sw(b, on);
			audio();
			toast(on ? _('Microphone on') : _('Microphone off: your NVR receives silence'));
		});
	};
	$('au-on').onclick = e => {
		const on = e.currentTarget.getAttribute('aria-checked') != 'true';
		sheet({
			title: on ? _('Turn audio on?') : _('Turn audio off?'),
			text: on ? _('Video restarts for a few seconds, and your NVR reconnects by itself.') : _('Video restarts for a few seconds, and your NVR reconnects by itself. The microphone, talk and the siren stop working until you turn it back on.'),
			ok: on ? _('Turn on') : _('Turn off'),
			icon: 'audio',
			run: async () => {
				let lost = 0;
				try {
					await api(M + 'config', { method: 'POST', json: { audio: on }, quiet: 1 });
				} catch (x) {
					/* The restart can take the answer with it, or the request never got there. */
					if (x.kind != 'off') throw x;
					lost = 1;
				}
				holdOff = Date.now() + 8e3;
				S.c && (S.c.audio = on);
				audio();
				toast(
					lost
						? _('The camera did not answer. Checking whether it took the change.')
						: on
							? _('Audio on: video is restarting')
							: _('Audio off: video is restarting')
				);
				setTimeout(
					() =>
						config()
							.then(() => {
								if (!lost || !S.c) return;
								toast(
									S.c.audio == on
										? on
											? _('It did: audio is on.')
											: _('It did: audio is off.')
										: on
											? _('It did not: audio is still off.')
											: _('It did not: audio is still on.')
										);
							})
							.catch(noop),
					6e3
				);
			}
		});
	};
	/* G.711 A-law (ITU reference coder): 0→0xD5, -1→0x55, 32767→0xAA, -32768→0x2A */
	function alaw(s) {
		let m = 213,
			g = 0;
		s >>= 3;
		if (s < 0) {
			m = 85;
			s = -s - 1;
		}
		while (g < 8 && s > (32 << g) - 1) g++;
		return (g > 7 ? 127 : (g << 4) | ((g < 2 ? s >> 1 : s >> g) & 15)) ^ m;
	}
	/* Talk: hold to talk, or (latch) tap to start and tap to stop; the choice is per browser. */
	const T = { st: 'idle', q: [], n: 0, tk: 0 },
		TB = [];
	let latch = ls.get(LATCH) === true;
	$$('.talk-box').forEach(x => {
		const b = h(
				'button',
				{ class: 'talk', 'aria-pressed': 'false' },
				ico('mic'),
				h('span', null, h('span'), h('small')),
				h('span', { class: 'mono' })
			),
			m = bars(16),
			st = h('span', { role: 'status' });
		m.setAttribute('aria-label', _('Your microphone level'));
		m.hidden = true;
		x.append(b, m, st);
		TB.push([b, st, m]);
		hold(
			b,
			() => (latch && T.st != 'idle' ? talkStop() : talkGo()),
			() => latch || talkStop()
		);
	});
	$('au-latch').checked = latch;
	$('au-latch').onchange = e => {
		latch = e.target.checked;
		ls.set(LATCH, latch || null);
		talkUI();
	};
	function talkUI(e) {
		if (e != null) T.err = e;
		const on = T.st == 'on' || T.st == 'stopping',
			w = T.st == 'starting',
			v = S.m?.audio?.speaker?.volume,
			en = aon();
		for (const [b, st, m] of TB) {
			const [a, c] = b.children[1].children;
			b.setAttribute('aria-pressed', on);
			b.disabled = en === false && T.st == 'idle';
			a.textContent = w ? _('Starting…') : on ? _('Talking') : latch ? _('Tap to talk') : _('Hold to talk');
			c.textContent = on ? (latch ? _('Tap to stop') : _('Release to stop')) : '';
			m.hidden = T.st != 'on';
			on || (b.children[2].textContent = '');
			st.textContent =
				T.err ||
				(on
					? v != null
						? _('Your voice plays through the speaker of {n} at volume {v}.', { n: label(), v })
						: _('Your voice plays through the speaker of {n}.', { n: label() })
						: w
						? _('Waiting for the microphone…')
						: en === false
							? _('Audio is off on the camera.')
							: '');
		}
	}
	async function talkGo() {
		if (T.st != 'idle' || !csrf || off) return;
		const AC = window.AudioContext || window.webkitAudioContext,
			tk = ++T.tk;
		if (!isSecureContext || !navigator.mediaDevices?.getUserMedia || !AC)
			return talkUI(_('Talk needs the HTTPS page and a browser microphone.'));
		Object.assign(T, { st: 'starting', q: [], n: 0, ok: 0, n409: 0, err: '' });
		talkUI();
		try {
			const ctx = (T.ctx = new AC()),
				s = await navigator.mediaDevices.getUserMedia({
					audio: { channelCount: 1, echoCancellation: true, noiseSuppression: true, autoGainControl: true }
				});
			if (tk != T.tk || T.st != 'starting') {
				s.getTracks().forEach(t => t.stop());
				ctx.close();
				return;
			}
			T.s = s;
			await ctx.resume();
			const p = (T.p = ctx.createScriptProcessor(4096, 1, 1)),
				g = ctx.createGain(),
				r = ctx.sampleRate / 8e3;
			let acc = 0,
				cnt = 0,
				ph = 0;
			g.gain.value = 0;
			ctx.createMediaStreamSource(s).connect(p);
			p.connect(g);
			g.connect(ctx.destination);
			p.onaudioprocess = ev => {
				if (T.st != 'on') return;
				const x = ev.inputBuffer.getChannelData(0),
					o = new Uint8Array((x.length / r + 1) | 0);
				let k = 0,
					e2 = 0;
				for (const v0 of x) {
					e2 += v0 * v0;
					acc += v0;
					cnt++;
					if (++ph >= r) {
						ph -= r;
						const v = Math.max(-1, Math.min(1, acc / cnt));
						acc = cnt = 0;
						o[k++] = alaw(Math.round(v * 32767));
					}
				}
				/* The level meter shows this browser's own samples (RMS, dBFS). */
				const db = 10 * Math.log10(e2 / x.length || 1e-10);
				for (const [, , m] of TB) level(m, db);
				T.q.push(o.subarray(0, k));
				T.n += k;
				while (T.n > 8e3) T.n -= T.q.shift().length;
				pump();
			};
			T.st = 'on';
			T.t0 = Date.now();
			clearInterval(T.iv);
			T.iv = setInterval(timer, 500);
			talkUI();
		} catch (e) {
			tk == T.tk &&
				end(
					e?.name == 'NotAllowedError'
						? _('The browser blocked the microphone. Allow it for this page.')
						: e?.name == 'NotFoundError'
							? _('This device has no microphone.')
							: _('The microphone could not start.')
				);
		}
	}
	function timer() {
		const s = ((Date.now() - T.t0) / 1e3) | 0;
		for (const [b] of TB) b.children[2].textContent = ((s / 60) | 0) + ':' + p2(s % 60);
	}
	async function pump() {
		if (T.sending) return;
		T.sending = 1;
		try {
			while (T.n && (T.n >= 2e3 || T.st == 'stopping')) {
				const n = Math.min(T.n, 4e3),
					o = new Uint8Array(n);
				for (let k = 0; k < n;) {
					const c = T.q[0],
						t = Math.min(c.length, n - k);
					o.set(c.subarray(0, t), k);
					k += t;
					t == c.length ? T.q.shift() : (T.q[0] = c.subarray(t));
				}
				T.n -= n;
				try {
					await api(M + 'talk', { method: 'POST', body: o, type: 'audio/PCMA', timeout: 4e3 });
					T.ok = 1;
					T.n409 = 0;
				} catch (e) {
					if (e.st == 409 && T.ok && !/another/.test(e.raw) && ++T.n409 < 4) continue;
					end(e.kind == 'off' ? _('The camera stopped answering.') : e.message);
					break;
				}
			}
		} finally {
			T.sending = 0;
			if (T.st == 'stopping' && !T.n) {
				T.st = 'idle';
				talkUI();
			}
		}
	}
	function tclose() {
		clearInterval(T.iv);
		try {
			T.s?.getTracks().forEach(t => t.stop());
			T.p && (T.p.onaudioprocess = null);
			T.ctx?.close();
		} catch {}
		T.s = T.p = T.ctx = null;
	}
	function talkStop() {
		if (T.st == 'starting') {
			T.st = 'idle';
			tclose();
			talkUI();
		}
		if (T.st != 'on') return;
		tclose();
		T.st = 'stopping';
		talkUI();
		pump();
	}
	function end(m) {
		tclose();
		Object.assign(T, { st: 'idle', q: [], n: 0 });
		talkUI(m || '');
	}
	$('au-tone').onclick = act(async () => {
		if (T.st != 'idle') no(_('Stop talking first.'));
		const b = new Uint8Array(8e3);
		for (let i = 0; i < 8e3; i++)
			b[i] = alaw(Math.round(Math.sin(i * 0.34558) * 9e3 * Math.min(1, i / 240, (8e3 - i) / 240)));
		await api(M + 'talk', { method: 'POST', body: b, type: 'audio/PCMA', timeout: 5e3 });
		toast(_('Playing a 1-second test tone'));
		await new Promise(r => setTimeout(r, 1e3));
	});
	document.addEventListener('visibilitychange', () => {
		if (document.hidden) {
			talkStop();
			ptzEnd();
		} else csrf && !off && load(route);
	});
	addEventListener('pagehide', () => {
		talkStop();
		ptzEnd();
	});
	/* Names for snippets: "camera1" → camera1 (Home Assistant) and FRIGATE_CAMERA1_PASSWORD (Frigate). */
	const slug = () =>
		name()
			.toLowerCase()
			.replace(/[^a-z0-9]+/g, '_')
			.replace(/^_+|_+$/g, '') || 'camera';
	const envName = () => `FRIGATE_${slug().toUpperCase()}_PASSWORD`;
	const FO = { restream: 1, onvif: 1, fixed: 1 }; // Frigate snippet options
	function streams() {
		const c = S.c;
		if (!c) return;
		const H = [...new Set([S.q?.wifi?.address, c.bind, location.hostname, S.n?.address].filter(x => x && x != '0.0.0.0'))],
			el = $('st-host'),
			rp = c.rtsp_port || 18554;
		if (!H.includes(hs)) hs = H[0];
		if (el.dataset.k != H) {
			el.dataset.k = H;
			el.replaceChildren(...H.map(x => h('button', { value: x, class: 'mono' }, x)));
		}
		pick(el, hs);
		const ln = (nm, ch) =>
			h(
				'div',
				{ class: 'stack s' },
				h('div', null, h('h3', { text: nm }), h('small', { class: 'mono', text: spec(ch) })),
				h(
					'div',
					{ class: 'f' },
					h('span', { class: 'lbl', text: 'RTSP' }),
					cf(`rtsp://${hs}:${rp}/live/ch${ch}`, _('{lens} RTSP address', { lens: nm }))
				),
				h(
					'div',
					{ class: 'f' },
					h('span', { class: 'lbl', text: _('Still') }),
					cf(`https://${hs}:18443/api/v1/snapshot?channel=${ch}`, _('{lens} still address', { lens: nm }))
				)
			);
		$('st-lenses').replaceChildren(ln(_('Pan/tilt lens'), 0), ...(c.sensors > 1 ? [ln(_('Fixed lens'), 2)] : []));
		$('st-onvif').replaceChildren(cf(`https://${hs}:18443/onvif/device_service`, _('ONVIF device address')));
		$('st-ont').replaceChildren(
			tip(
				_('ONVIF device address'),
				_('Pan/tilt and 16 presets over ONVIF.') +
					' ' +
					(c.discovery ? _('Discovery is on.') : _('Discovery is off: add the camera by address.')) +
					(c.main_only ? ' ' + _('ch1 and ch3 repeat ch0 and ch2.') : '')
			)
		);
		$('st-user').replaceChildren(cf(c.username, _('stream user name')));
		$('st-pwr').hidden = !c.password_set;
		$('st-pwn').hidden = !!c.password_set;
		ha(c);
		$('st-o3l').hidden = c.sensors < 2;
		$('st-env').replaceChildren(...parts(_('Set {v} in Frigate’s environment to the stream password.'), { v: h('code', { text: envName() }) }));
		tx('st-yaml', yaml(rp, c));
	}
	seg($('st-host'), v => {
		hs = v;
		streams();
	});
	$('st-copy').onclick = e => copy($('st-yaml').textContent, e.currentTarget, $('st-yaml'));
	[
		['st-o1', 'restream'],
		['st-o2', 'onvif'],
		['st-o3', 'fixed']
	].forEach(([id, k]) => {
		$(id).checked = !!FO[k];
		$(id).onchange = e => {
			FO[k] = e.target.checked;
			streams();
		};
	});
	/* The stream password: masked; fetched from the camera only to show or copy it,
	   and forgotten when leaving the page. */
	const MASK = '••••••••••••',
		pwShow = $('st-pws'),
		pwCopy = $('st-pwc');
	async function getPw() {
		if (spw == null) spw = (await api(M + 'config/password')).password;
		return spw;
	}
	function hidePw() {
		spw = null;
		tx('st-pwv', MASK);
		pwShow.setAttribute('aria-pressed', 'false');
		pwShow.setAttribute('aria-label', _('Show the stream password'));
		pwShow.firstChild.textContent = _('Show');
	}
	pwShow.onclick = () =>
		run(pwShow, async () => {
			if (pwShow.getAttribute('aria-pressed') == 'true') return hidePw();
			tx('st-pwv', await getPw());
			pwShow.setAttribute('aria-pressed', 'true');
			pwShow.setAttribute('aria-label', _('Hide the stream password'));
			pwShow.firstChild.textContent = _('Hide');
		});
	pwCopy.onclick = () => run(pwCopy, async () => copy(await getPw(), pwCopy));
	/* The siren: its sound, volume and length are kept on the camera, it asks before it
	   sounds, and it stops by itself. */
	const SIR = [
			['alarm', _('Alarm'), _('Rising and falling tone')],
			['two_tone', _('Two-tone'), _('Alternating high and low')],
			['pulse', _('Pulse'), _('Short beeps')],
			['chime', _('Chime'), _('Soft, for a doorbell or a reminder')]
		],
		sirOn = () => S.m?.audio?.speaker?.siren,
		sirName = v => SIR.find(x => x[0] == v)?.[1] || v,
		sirLeft = () => Math.max(0, (sirOn()?.left_s || 0) - Math.round(age('m') / 1e3));
	function sirUI() {
		const on = sirOn(),
			b = $('ov-sir'),
			talking = T.st != 'idle';
		b.classList.toggle('dzf', !!on);
		b.lastChild.textContent = on ? _('Stop the siren · {a} s', { a: sirLeft() }) : talking ? _('Siren (after talk)') : _('Siren…');
		b.disabled = !on && (talking || aon() === false);
		$('ov-sirn').hidden = !!on || aon() !== false;
		const flashing = left(S.x?.live?.strobe_left_s || 0);
		$('au-sirx').hidden = !on && !flashing;
		if (!on && flashing) {
			b.lastChild.textContent = _('Stop the flashing · {flashing} s', { flashing });
			b.classList.add('dzf');
			b.disabled = false;
		}
		tx('au-sirn', on ? _('{s}: {a} s left.', { s: sirName(on.sound), a: sirLeft() }) : flashing ? _('The white light is flashing: {flashing} s left.', { flashing }) : '');
	}
	function sirShow(x) {
		$('au-sir').hidden = !x;
		if (!x) return;
		$$('input', $('au-sirs')).forEach(i => busy(i) || (i.checked = i.value == x.sound));
		setSl('au-sirv', x.volume);
		durs($('au-sird'), [10, 30, 60, 120, 300], x.seconds);
		busy($('au-sirf')) || ($('au-sirf').checked = x.strobe);
	}
	const putS = (b, siren) => run(b, () => putX({ siren }), $('au-sirn'));
	$('au-sirs').append(
		...SIR.map(([v, nm, d], i) =>
			h(
				'div',
				{ class: 'sir' },
				h(
					'label',
					{ class: 'chk' },
					h('input', { type: 'radio', name: 'au-sirs', value: v, onchange: e => putS(e.target, { sound: v }) }),
					h('span', null, h('b', { text: nm }), h('small', { text: d }))
				),
				h(
					'button',
					{
						type: 'button',
						class: 'btn',
						'aria-label': _('Test {nm} for 3 seconds', { nm }),
						onclick: () =>
							sheet({
								title: _('Test {nm}?', { nm }),
								text: _('It plays for 3 seconds at {a}% volume, as loud as the real thing.', { a: S.x?.siren?.volume ?? 100 }),
								ok: _('Play it'),
								icon: 'audio',
								run: async () => {
									await ctl('siren_test', i);
									toast(_('Testing {nm} for 3 seconds', { nm }));
									await media();
								}
							})
					},
					_('Test 3 s')
				)
			)
		)
	);
	$('au-sirl').append(slider('au-sirv', _('Siren volume'), 1, 100, 100, v => v + '%', v => putX({ siren: { volume: v } })));
	$('au-sird').onchange = e => putS(e.target, { seconds: +e.target.value });
	$('au-sirf').onchange = e => putS(e.target, { strobe: e.target.checked });
	const sirStop = act(async () => {
		await ctl(sirOn() ? 'siren' : 'strobe', 0);
		S.x?.live && (S.x.live.strobe_left_s = 0);
		await media();
	});
	$('au-sirx').onclick = sirStop;
	$('at-sirb').onclick = sirStop;
	$('ov-sir').onclick = e => {
		if (sirOn() || left(S.x?.live?.strobe_left_s || 0)) return sirStop(e);
		const x = S.x?.siren;
		sheet({
			title: _('Sound the siren?'),
			text: x
				? _('{s} at {volume}%, for {t}. People nearby will hear it. You can stop it from here.', { s: sirName(x.sound), volume: x.volume, t: long(x.seconds) })
				: _('People nearby will hear it. You can stop it from here.'),
			ok: _('Sound the siren'),
			icon: 'siren',
			danger: 1,
			run: async () => {
				await ctl('siren', 1);
				await media();
			}
		});
	};
	/* Home Assistant: POST /api/v1/control over HTTPS with Basic sign-in (the stream account).
	   The timed white light is what Home Assistant calls on a Frigate person event. */
	const haUrl = () => `https://${hs}:18443/api/v1/control`,
		haBody = (operation, value) => JSON.stringify({ operation, value, value2: 0 }),
		HA = [
			[
				_('Siren'),
				'siren',
				1,
				_('the siren action'),
				() =>
					_('Sounds for {a} (set on Audio), or give value2 in seconds, up to 300. Send value 0 to stop it.', { a: S.x ? long(S.x.siren.seconds) : _('the time set') })
			],
			[_('White light'), 'white_light', 1, _('the white light action'), () => _('Send value 0 to turn it off.')],
			[
				_('White light for a while'),
				'white_light_trigger',
				0,
				_('the timed white light action'),
				() =>
					_('Value 0 keeps it on for {a} (set on Image & night), or give 1–3600 seconds; then it goes back to its schedule or manual state.', { a: S.x ? dur(S.x.white_light.trigger_s) : _('the time set') })
			],
			[_('Flashing light'), 'strobe', 30, _('the flashing light action'), () => _('The white light flashes for this many seconds, up to 300. Send value 0 to stop it.')],
			[_('Privacy mode'), 'privacy', 1, _('the privacy mode action'), () => _('Send value 0 to turn it off.')]
		];
	let haK = '';
	function ha(c) {
		const k = c.username + haUrl() + HA.map(x => x[4]());
		if (k == haK) return;
		haK = k;
		$('st-hau').replaceChildren(cf(haUrl(), _('Address for all actions')));
		$('st-acts').replaceChildren(
			...HA.map(([nm, op, v, what, note]) => {
				const body = haBody(op, v),
					curl = `curl -k -u ${c.username} -H 'Content-Type: application/json' -d '${body}' ${haUrl()}`,
					b = h(
						'button',
						{ class: 'btn', 'aria-label': _('Copy {what} as a curl command', { what }) },
						h('span', { text: _('Copy') })
					);
				b.onclick = () => copy(curl, b);
				return h(
					'li',
					null,
					h('span', { class: 'han' }, h('b', { text: nm }), tip(nm, note())),
					h('code', { class: 'body', dir: 'ltr', text: body }),
					b
				);
			})
		);
	}
	function rest(c) {
		const n = slug(),
			cmd = (name, body) => [
				`  ${n}_${name}:`,
				`    url: ${haUrl()}`,
				'    method: post',
				`    username: ${c.username}`,
				`    password: !secret ${n}_stream_password`,
				'    content_type: application/json',
				`    payload: '${body}'`,
				'    verify_ssl: false'
			];
		return (
			[
				`# Home Assistant configuration.yaml; put ${n}_stream_password in secrets.yaml.`,
				'rest_command:',
				...cmd('white_light_on', haBody('white_light', 1)),
				...cmd('white_light_off', haBody('white_light', 0)),
				...cmd('white_light_trigger', haBody('white_light_trigger', 0)),
				...cmd('siren_on', haBody('siren', 1)),
				...cmd('siren_off', haBody('siren', 0)),
				...cmd('flashing_light', haBody('strobe', 30)),
				...cmd('flashing_light_off', haBody('strobe', 0)),
				...cmd('privacy_on', haBody('privacy', 1)),
				...cmd('privacy_off', haBody('privacy', 0))
			].join('\n') + '\n'
		);
	}
	$('st-rc').onclick = e => S.c && copy(rest(S.c), e.currentTarget);
	/* Frigate: go2rtc restreams (with a talk stream when audio is on), or direct RTSP inputs. */
	function yaml(rp, c) {
		const a = c.audio,
			u = c.username,
			pw = `{${envName()}}`,
			rs = FO.restream,
			fx = c.sensors > 1 && FO.fixed,
			src = (ch, x = '') => `rtsp://${u}:${pw}@${hs}:${rp}/live/ch${ch}${x}`,
			bc = a ? '#backchannel=0' : '',
			cam = (n, ch, m) => [
				`  ${n}:`,
				'    ffmpeg:',
				...(a ? ['      output_args:', '        record: preset-record-generic-audio-aac'] : []),
				'      inputs:',
				...(rs
					? [`        - path: rtsp://127.0.0.1:8554/${n}`, '          input_args: preset-rtsp-restream']
					: [`        - path: ${src(ch)}`]),
				'          roles: [record, detect]',
				...(m && rs && a ? ['    live:', '      streams:', '        Main: jooan_main_talk'] : []),
				...(m && FO.onvif
					? [
							'    onvif:',
							`      host: https://${hs}`,
							'      port: 18443',
							`      user: ${u}`,
							`      password: "${pw}"`,
							'      ignore_time_mismatch: true', /* no RTC: its clock can drift past ONVIF's 5 minutes */
							'      profile: main0'
						]
					: [])
			];
		return (
			[
				...(rs
					? [
							'go2rtc:',
							'  streams:',
							'    jooan_main:',
							'      - ' + src(0, bc),
							...(a
								? [
										'    jooan_main_talk:',
										'      - rtsp://127.0.0.1:8554/jooan_main#backchannel=0',
										'      - ' + src(0, '#backchannel=1#media=audio')
									]
								: []),
							...(fx ? ['    jooan_fixed:', '      - ' + src(2, bc)] : [])
						]
					: []),
				'cameras:',
				...cam('jooan_main', 0, 1),
				...(fx ? cam('jooan_fixed', 2) : [])
			].join('\n') + '\n'
		);
	}
	function rQuality(q) {
		const w = q.wifi || {},
			e = q.ethernet || {},
			d = w.signal_dbm,
			r = w.rate_mbps;
		$('nw-now').replaceChildren(
			w.associated ? sigIcon(d) : h('span', { class: 'ico warn' }, ico('warn')),
			h(
				'span',
				null,
				h('b', { text: w.associated ? _('Connected to {ssid}', { ssid: w.ssid || _('Wi-Fi') }) : _('Not connected to Wi-Fi') }),
				h('small', {
					text: [w.band, d != null && mi(d) + ' dBm, ' + sig(d).toLocaleLowerCase(LC), r != null && +r + ' Mb/s']
						.filter(Boolean)
						.join(' · ')
				})
			)
		);
		tx('nw-ad', w.address || location.hostname);
		tx('nw-gw', w.gateway || '—');
		const st = (k, v, s) =>
			h('div', null, h('dt', { text: k }), h('dd', { text: v }), s && h('small', { text: s }));
		$('nw-st').replaceChildren(
			st(_('Sent'), bytes(w.tx_bytes)),
			st(_('Received'), bytes(w.rx_bytes)),
			st(
				_('Ethernet'),
				e.link ? (e.speed_mbps ? e.speed_mbps + ' Mb/s' : _('Connected')) : _('Not used')
			)
		);
		tx('nw-qc', _('Checked {t}', { t: hm(new Date(S['q@'] || Date.now())) }));
		tx(
			'se-net',
			w.associated
				? (w.ssid || _('Wi-Fi')) + ' · ' + (d != null ? _('{q} signal', { q: sig(d) }) : _('connected'))
				: _('Not connected to Wi-Fi')
		);
		joinHelp();
		ov();
	}
	$('nw-qa').onclick = act(() => quality());
	/* Signal bars: the same steps as sig()'s words. */
	const BARS = ['M2 20h3v-4H2z', 'M7 20h3v-8H7z', 'M12 20h3V8h-3z', 'M17 20h3V4h-3z'];
	function sigIcon(d) {
		const s = ico('off'),
			n = d == null ? 0 : d >= -50 ? 4 : d >= -60 ? 3 : d >= -70 ? 2 : 1;
		s.setAttribute('class', 'sig');
		s.innerHTML = BARS.map((p, i) => `<path d="${p}"${i < n ? '' : ' class="off"'}/>`).join('');
		return s;
	}
	/* Nearby networks: scanned when asked, since a scan takes 5-10 s. Hidden networks are not
	   listed, so a name can still be typed. The camera only joins networks with a password. */
	let nets = null,
		netsAll = 0,
		joinOpen = 0,
		joinName = null;
	const bandOf = f => (f ? (f >= 4900 ? _('5 GHz') : _('2.4 GHz')) : '');
	let scanOpen = 0;
	$('nw-sb').onclick = act(async () => {
		scanOpen = 1;
		$('nw-scan').hidden = false;
		$('nw-sb').setAttribute('aria-expanded', 'true');
		tx('nw-sh', _('Looking for networks; this takes a few seconds.'));
		try {
			nets = await api(J + 'network/wifi/scan', { timeout: 2e4 });
		} catch (e) {
			tx('nw-sh', '');
			throw e;
		}
		const at = new Date().toLocaleTimeString(LC, { hour: '2-digit', minute: '2-digit' });
		tx('nw-sc', _('Networks nearby · scanned {t}', { t: at }));
		$('nw-sb').querySelector('span').textContent = _('Scan again');
		rNets();
	}, $('nw-scan'));
	function rNets() {
		const A = Array.isArray(nets) ? nets : [],
			chosen = joinOpen && joinName,
			/* the strongest ten, the one in use and the chosen one; the rest on request */
			L = netsAll ? A : A.filter((n, k) => k < 10 || n.in_use || n.ssid == chosen);
		$('nw-scan').after($('nw-join'));
		$('nw-sc').hidden = !L.length;
		tx('nw-sh', L.length ? '' : _('No networks found. A hidden network can still be joined by name.'));
		$('nw-nets').hidden = !L.length;
		$('nw-nets').replaceChildren(
			...L.map(n => {
				const usable = n.secure && !n.in_use,
					li = h(
						'li',
						null,
						h(
							'button',
							{
								type: 'button',
								class: 'net',
								disabled: !usable,
								'aria-expanded': usable ? 'false' : null,
								onclick: () => joinForm(n.ssid, li)
							},
							sigIcon(n.signal_dbm),
							h(
								'span',
								null,
								h('b', { text: n.ssid }),
								h('small', {
									text: [
										bandOf(n.frequency_mhz),
										sig(n.signal_dbm).toLowerCase(),
										!n.secure && _('no password, so the camera can’t join it')
									]
										.filter(Boolean)
										.join(' · ')
								})
							),
							n.in_use && pill('ok', _('In use')),
							n.secure && ico('lock')
						)
					);
				if (chosen == n.ssid && usable) joinForm(n.ssid, li, 1);
				return li;
			})
		);
		A.length > L.length &&
			$('nw-nets').append(
				h(
					'li',
					null,
					h('button', { type: 'button', class: 'lnk', onclick: () => ((netsAll = 1), rNets()) }, _('Show {a} more', { a: A.length - L.length }))
				)
			);
		if (chosen && !L.some(n => n.ssid == chosen && n.secure && !n.in_use)) joinClose();
	}
	/* The join form opens under the chosen network, or below the list for a typed name. A name
	   with a "?" may hide characters the scan cannot show, so it stays editable. */
	function joinForm(id, li, quiet) {
		const f = $('nw-join');
		if (!quiet && joinOpen && joinName == id && id != null) return joinClose();
		joinClose();
		joinOpen = 1;
		joinName = id;
		const typed = id == null || id.includes('?');
		$('nw-ssid').value = id || '';
		$('nw-sf').hidden = !typed;
		tx('nw-pl', id ? _('Password for {id}', { id }) : _('Password'));
		joinHelp();
		if (li) {
			li.classList.add('open');
			li.firstChild.setAttribute('aria-expanded', 'true');
			li.append(f);
		}
		f.hidden = false;
		quiet || (typed ? $('nw-ssid') : $('nw-psk')).focus();
	}
	function joinClose() {
		const f = $('nw-join');
		joinOpen = 0;
		joinName = null;
		f.hidden = true;
		$('nw-psk').value = '';
		clr($('nw-psk'));
		clr($('nw-ssid'));
		$$('.nets .open').forEach(li => {
			li.classList.remove('open');
			li.firstChild.setAttribute('aria-expanded', 'false');
		});
		$('nw-scan').after(f);
	}
	$('nw-hid').onclick = () => joinForm(null);
	$('nw-jc').onclick = () => {
		joinClose();
		$('nw-hid').focus();
	};
	function joinHelp() {
		if (joining) return;
		const cur = S.q?.wifi?.ssid || _('its network'),
			to = joinName || $('nw-ssid').value.trim() || _('the new one');
		tx(
			'nw-help',
			_('The camera leaves {cur} and tries {to}. You then have 90 seconds to confirm from a device that can reach it there. If nobody confirms, it goes back to {cur} and restarts.', { cur, to })
		);
	}
	$('nw-ssid').addEventListener('input', joinHelp);
	$('nw-join').onsubmit = e => {
		e.preventDefault();
		const s = $('nw-ssid'),
			p = $('nw-psk'),
			id = s.value,
			k = p.value,
			safe = v => /^[ !#-[\]-~]*$/.test(v), // what the camera's Wi-Fi setup takes: plain ASCII, no " or \
			bad = (el, m) => {
				el.setAttribute('aria-invalid', 'true');
				err(el, m);
				el.focus();
			};
		if (!id.trim() || id.length > 32) return bad(s, _('Enter the network name, up to 32 characters.'));
		if (!safe(id)) return bad(s, _('This camera can only join a network whose name uses plain English letters, digits and symbols: no accents, no other alphabets, and no " or \\.'));
		if (k.length < 8 || k.length > 63) return bad(p, _('Wi-Fi passwords have 8 to 63 characters.'));
		if (!safe(k)) return bad(p, _('The camera can only use plain letters, digits and symbols here, without " or \\.'));
		run(
			sb(e),
			async () => {
				const t0 = Date.now(),
					prev = S.q?.wifi?.ssid || '';
				let d = null;
				joining = 1;
				tx('nw-help', _('Joining {id}. This can take up to a minute; keep this page open.', { id }));
				try {
					d = await api(J + 'network/wifi', {
						method: 'POST',
						json: { ssid: id, password: k },
						timeout: 75e3,
						quiet: 1
					});
				} catch (x) {
					if (x.kind != 'off') {
						/* refused: any countdown picked up meanwhile is over */
						W = null;
						ls.set(WIFI);
						wifi();
						shell();
						if (x.st == 409) no(IP_NO[x.d?.reason] || _('The camera can’t change networks right now.'));
						throw x;
					}
				} finally {
					joining = 0;
				}
				/* The camera's own countdown takes over at the next read; until then, from the request. */
				const sec = d && (d.deadline_seconds ?? d.seconds_left);
				W = {
					...(W && !W.end ? W : {}),
					id: d?.id || W?.id || '',
					ssid: id,
					prev,
					was: S.q?.wifi?.address,
					deadline: sec > 0 ? Date.now() + sec * 1e3 : W && !W.end ? W.deadline : t0 + 9e4,
					lost: !d
				};
				ls.set(WIFI, W);
				joinClose();
				wifi();
				shell();
				$('wc-h').focus();
				d ? wifiState().catch(wifiLost) : goOff();
			},
			p
		);
	};
	/* GET network/wifi: the camera's view of a trial, so any device can see and answer it. */
	function rWifi(d) {
		if (d?.pending) {
			const fresh = !W || W.end;
			if (fresh) W = { id: '', ssid: d.static || d.ssid || _('the new network'), prev: '', ip: !!d.static };
			W.joined = d.joined;
			W.addr = d.address;
			W.lost = false;
			if (d.seconds_left != null) {
				const dl = Date.now() + d.seconds_left * 1e3;
				if (!W.deadline || Math.abs(dl - W.deadline) > 2e3) W.deadline = dl;
			} else W.deadline = W.deadline || Date.now() + 9e4;
			ls.set(WIFI, W);
			joining || joinClose(); // if the request is refused, its form comes back with the reason
			wifi();
			place();
			shell();
		} else if (W && !W.end && !joining) {
			const pv = prevName(),
				n = label();
			d?.last == 'committed'
				? wend(_('{n} stays on {ssid}', { n, ssid: W.ssid }), _('If the camera’s address changed, update your NVR.'))
				: d?.last == 'rolled_back'
					? wend(
							_('{n} went back to {pv}', { n, pv }),
							_('Nobody confirmed {ssid} in time, so the camera went back and restarted.', { ssid: W.ssid })
						)
					: wend(_('The test is over'), _('{n} is no longer testing {ssid}.', { n, ssid: W.ssid }));
			quality().catch(noop);
			ipInfo().catch(noop);
		}
	}
	/* The camera can't be asked: fall back to counting here, from the request or the last answer. */
	function wifiLost(e) {
		if (e?.kind == 'off' && W && !W.end && !joining) {
			W.lost = true;
			wifi();
		}
	}
	const trialId = () => W.id || 'current', // the camera wants an id but acts on the one running trial
		prevName = () => W.prev || (W.ip ? _('the previous address') : _('the previous network'));
	function wifi() {
		$('p-network').classList.toggle('wc', !!W);
		$('nw-sb').hidden = !!W;
		$('nw-scan').hidden = !!W || !scanOpen;
		$('nw-join').hidden = !!W || !joinOpen;
		$('nw-count').hidden = !W;
		if (!W) return;
		const L = Math.max(0, W.deadline - Date.now()),
			s = Math.ceil(L / 1e3),
			pv = prevName(),
			n = label(),
			E = W.end,
			was = W.was || location.hostname,
			u = W.addr && `https://${W.addr}${location.port ? ':' + location.port : ''}/`;
		if (!E && !L) return wend(_('Time is up'), _('{n} goes back to {pv} and restarts (about 90 s).', { n, pv }));
		tx('wc-h', E ? W.eT : _('Keep {ssid}?', { ssid: W.ssid }));
		tx('wc-n', ((s / 60) | 0) + ':' + p2(s % 60));
		$('wc-t').setAttribute('aria-label', _('{t} left to decide', { t: dur(s) }));
		$('wc-t').hidden = !!E;
		$('wc-arc').style.strokeDashoffset = 452.4 * (1 - L / 9e4);
		tx(
			'wc-p',
			E
				? W.eP
				: W.lost
					? W.ip
						? W.addr
							? _('The camera moved. To keep {ssid}, open it at the new address and sign in again in time; otherwise it goes back to {pv} and restarts.', { ssid: W.ssid, pv })
							: _('The camera moved. To keep {ssid}, open it at {n}.local, or the address your router lists, and sign in again in time; otherwise it goes back to {pv} and restarts.', { ssid: W.ssid, n, pv })
							: _('The camera can’t be reached from this device. To keep {ssid}, open this page from a device on it in time; otherwise it goes back to {pv} and restarts.', { ssid: W.ssid, pv })
					: W.joined
						? _('{n} is answering on {ssid}. If you do nothing, it goes back to {pv} and restarts when the timer ends.', { n, ssid: W.ssid, pv })
						: _('{n} is trying {ssid}. If nobody confirms, it goes back to {pv} and restarts.', { n, ssid: W.ssid, pv })
		);
		/* the steps, from the camera's own answer */
		const q = S.q?.wifi,
			steps =
				E || W.joined == null
					? []
					: [
							...(W.ip
								? []
								: [
										[
											W.joined ? 'ok' : 'wait',
											W.joined ? _('Joined {ssid}', { ssid: W.ssid }) : _('Joining {ssid}', { ssid: W.ssid }),
											W.joined && q?.ssid == W.ssid
												? [q.band, q.signal_dbm != null && mi(q.signal_dbm) + ' dBm'].filter(Boolean).join(' · ')
												: ''
										]
									]),
							[W.addr ? 'ok' : 'off', W.addr ? _('Got an address') : _('Waiting for an address'), W.addr || ''],
							...(W.joined && W.addr ? [['wait', _('Waiting for you'), '']] : [])
						],
			sk = JSON.stringify(steps);
		$('wc-st').hidden = !steps.length;
		if ($('wc-st').dataset.k != sk) {
			$('wc-st').dataset.k = sk;
			$('wc-st').replaceChildren(...hlist(steps));
		}
		const nk = [W.addr, was, !!E].join();
		if ($('wc-nt').dataset.k != nk) {
			$('wc-nt').dataset.k = nk;
			W.addr && W.addr != was
				? $('wc-nt').replaceChildren(
						...parts(
							W.ip
								? _('The address changes to {a}. Your NVR still points at {was}, so update it after you keep this address.', { was })
								: _('The address changes to {a}. Your NVR still points at {was}, so update it after you keep this network.', { was }),
							{ a: h('a', { href: u, class: 'mono', text: W.addr }) }
						)
					)
					: tx('wc-nt', W.ip ? _('If the address changes, update your NVR after keeping it.') : _('If the address changes, update your NVR after keeping the new network.'));
		}
		tx('wc-keep', E ? _('Done') : _('Keep {ssid}', { ssid: W.ssid }));
		tx('wc-back', _('Go back to {pv} now', { pv }));
		/* Nothing to answer while the camera is still joining, or from a device that can't reach it. */
		$('wc-keep').hidden = !E && (!!W.lost || !!joining || W.joined === false);
		$('wc-back').hidden = !!E || !!W.lost || !!joining;
	}
	function wend(t, p) {
		Object.assign(W, { end: 1, eT: t, eP: p });
		ls.set(WIFI);
		wifi();
		shell();
	}
	$('wc-keep').onclick = e => {
		if (W.end) {
			W = null;
			wifi();
			place();
			shell();
			return $('nw-hw').focus();
		}
		run(e.currentTarget, async () => {
			await api(J + 'network/wifi', { method: 'PUT', json: { id: trialId() }, timeout: 2e4 });
			wend(_('{n} stays on {ssid}', { n: label(), ssid: W.ssid }), _('If the camera’s address changed, update your NVR.'));
			quality().catch(noop);
			ipInfo().catch(noop);
		});
	};
	$('wc-back').onclick = () => {
		const pv = prevName();
		sheet({
			title: _('Go back to {pv} now?', { pv }),
			text: _('The camera drops {ssid} and restarts (about 90 s).', { ssid: W.ssid }),
			ok: _('Go back'),
			danger: 1,
			icon: 'network',
			run: async () => {
				await api(J + 'network/wifi', { method: 'DELETE', json: { id: trialId() }, timeout: 2e4 });
				wend(_('Going back to {pv}', { pv }), _('The camera is restarting; this page reconnects by itself.'));
			}
		});
	};
	/* IP address: automatic or static, through the same trial as a new network. */
	const IP_NO = {
		'address-in-use': _('Something on the network already answers on that address. Choose another.'),
		'gateway-silent': _('That gateway does not answer, so nothing beyond this network could reach the camera. Check it and try again.'),
		'no-network': _('The camera has no saved Wi-Fi network yet. Join one first.'),
		update: _('An update is being installed. Try again when it has finished.'),
		'wifi-trial': _('A network change is already being tested. Keep it or go back first.'),
		'boot-trial': _('A new release is still being tried out after an update. Try again in a few minutes.'),
		'no-runtime': _('A platform update stopped half way. Look at the camera over SSH first (see docs/RECOVERY.md).'),
		starting: _('The camera is still checking its start. Try again in about 4 minutes.')
	};
	function ipShow(mode) {
		$$('button', $('nw-ipm')).forEach(b => b.setAttribute('aria-checked', b.value == mode));
		$('nw-ipf').hidden = mode != 'static';
	}
	function rIp(d) {
		if (!$('nw-ipf').contains(document.activeElement)) {
			ipShow(d.mode);
			setIn($('nw-ipa'), d.address || '');
			setIn($('nw-ipp'), d.prefix == null ? '24' : String(d.prefix));
			setIn($('nw-ipg'), d.gateway || '');
		}
	}
	async function ipTrial(json, label) {
		const t0 = Date.now(),
			prev = S.q?.wifi?.address || location.hostname;
		let d = null;
		joining = 1;
		try {
			/* Checks and change take seconds; no answer by then means the camera moved and the reply can't reach here. */
			d = await api(J + 'network/ip', { method: 'PUT', json, timeout: 12e3, quiet: 1 });
		} catch (x) {
			if (x.st == 409) no(IP_NO[x.d?.reason] || _('The camera can’t change its address right now.'));
			if (x.kind != 'off') throw x;
		} finally {
			joining = 0;
		}
		W = { id: '', ssid: label, prev, ip: 1, was: prev, addr: json.address, deadline: t0 + 9e4, lost: !d };
		ls.set(WIFI, W);
		joinClose();
		wifi();
		shell();
		$('wc-h').focus();
		d ? wifiState().catch(wifiLost) : goOff();
	}
	seg($('nw-ipm'), v => {
		if (v == 'static') return ipShow(v);
		if (S.i?.mode != 'static') return ipShow(v);
		sheet({
			title: _('Go back to an automatic address?'),
			text: _('The router assigns the address again, which may be a different one. You then have 90 seconds to keep it.'),
			ok: _('Test automatic'),
			icon: 'network',
			run: () => ipTrial({ mode: 'dhcp' }, _('the automatic address'))
		});
	});
	$('nw-ipc').onclick = () => ipShow(S.i?.mode || 'dhcp');
	$('nw-ipf').onsubmit = e => {
		e.preventDefault();
		const ip = i => /^\d{1,3}(\.\d{1,3}){3}$/.test(i.value.trim()) && i.value.trim().split('.').every(n => +n < 256),
			a = $('nw-ipa'),
			p = $('nw-ipp'),
			g = $('nw-ipg'),
			bad = (el, m) => {
				el.setAttribute('aria-invalid', 'true');
				err(el, m);
				el.focus();
			};
		if (!ip(a)) return bad(a, _('Enter an address such as 192.168.1.50.'));
		if (!/^\d{1,2}$/.test(p.value.trim()) || +p.value < 8 || +p.value > 30) return bad(p, _('Enter a number from 8 to 30.'));
		if (!ip(g)) return bad(g, _('Enter the gateway’s address, such as 192.168.1.1.'));
		run(
			sb(e),
			() =>
				ipTrial({ mode: 'static', address: a.value.trim(), prefix: p.value.trim(), gateway: g.value.trim() }, a.value.trim()),
			$('nw-ipf')
		);
	};
	$('nw-name').onsubmit = e => {
		e.preventDefault();
		const i = $('nw-host'),
			v = i.value.trim();
		if (!/^[a-z0-9](?:[a-z0-9-]{0,61}[a-z0-9])?$/i.test(v.replace(/\.local$/i, '')))
			return bad(i, CODES.invalid_hostname);
		run(
			sb(e),
			async () => {
				const d = await api(J + 'network/mdns', { method: 'PUT', json: { hostname: v } }),
					u = `https://${d.address}${location.port ? ':' + location.port : ''}/`;
				$('nw-res').replaceChildren(...parts(_('Saved. {u} works after the next restart.'), { u: h('a', { href: u, class: 'mono', text: u }) }));
			},
			i
		);
	};
	/* What the platform accepts: RFC 1918 IPv4 (prefix 8-32) or ULA IPv6 (fc00::/7). 0 = valid but public. */
	function cidr(v) {
		const m = /^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})\/(\d{1,2})$/.exec(v);
		if (m) {
			const [a, b, c, d, p] = m.slice(1).map(Number);
			if (Math.max(a, b, c, d) > 255 || p < 8 || p > 32) return false;
			return a == 10 || (a == 172 && (b & 240) == 16) || (a == 192 && b == 168) ? 1 : 0;
		}
		const x = /^([0-9a-f:]*:[0-9a-f:]*)\/(\d{1,3})$/i.exec(v);
		if (!x || x[2] < 7 || x[2] > 128) return false;
		return /^f[cd][0-9a-f]{2}:/i.test(x[1]) ? 1 : 0;
	}
	const putR = L =>
		api(J + 'network/routes', { method: 'PUT', json: { cidrs: L.join(', ') }, timeout: 2e4 }).then(routes);
	function rRoutes(d) {
		const L = d?.cidrs || [];
		$('nw-routes').replaceChildren(
			...(L.length
				? L.map(c =>
						h(
							'li',
							null,
							h('code', { text: c }),
							h(
								'button',
								{
									class: 'btn',
									'aria-label': _('Remove {c}', { c }),
									onclick: e =>
										L.length < 2
											? err(e.currentTarget, _('The list cannot be empty: add the replacement first.'))
											: sheet({
													title: _('Remove {c}?', { c }),
													text: _('Devices on {c} lose access to the camera, this one too if it is on {c}.', { c }),
													ok: _('Remove'),
													danger: 1,
													icon: 'network',
													run: async () => {
														await putR(L.filter(x => x != c));
														toast(_('Removed {c}', { c }));
													}
												})
								},
								_('Remove')
							)
						)
					)
				: [h('li', { class: 'none', text: _('None: only the camera’s own network.') })])
		);
	}
	$('nw-add').onsubmit = e => {
		e.preventDefault();
		const i = $('nw-cidr'),
			v = i.value.trim(),
			ok = cidr(v);
		if (!ok)
			return bad(i, ok === 0 ? _('{v} is a public network.', { v }) + ' ' + CODES.invalid_routes : CODES.invalid_routes);
		if (S.r?.cidrs?.includes(v)) return bad(i, _('{v} is already in the list.', { v }));
		run(
			sb(e),
			async () => {
				await putR([...(S.r?.cidrs || []).filter(x => x != v), v]);
				i.value = '';
				toast(_('Added {v}', { v }));
			},
			i
		);
	};
	const TZ = [...Array(27).keys()]
			.map(i => i * 60 - 720)
			.concat(-570, -210, 210, 270, 330, 345, 390, 525, 570, 630, 765)
			.sort((a, b) => a - b),
		dz = -new Date().getTimezoneOffset(),
		hh = m => p2((Math.abs(m) / 60) | 0) + ':' + p2(Math.abs(m) % 60);
	TZ.includes(dz) || (TZ.push(dz) && TZ.sort((a, b) => a - b));
	$('sy-tz').append(
		...TZ.map(m =>
			h(
				'option',
				{ value: 'GMT' + (m < 0 ? '-' : '+') + hh(m), selected: m == dz },
				m == dz ? _('{z} (this device)', { z: 'UTC' + (m < 0 ? '−' : '+') + hh(m) }) : 'UTC' + (m < 0 ? '−' : '+') + hh(m)
			)
		)
	);
	$('sy-tz').onchange = () => (tzd = 1);
	/* With a time zone of its own, the camera's is what the list shows, not this device's. */
	function syncZone(x) {
		if (x.live.zone != 'camera' || tzd || Date.now() < tzAt) return;
		const m = x.utc_offset_min,
			v = 'GMT' + (m < 0 ? '-' : '+') + hh(m),
			el = $('sy-tz');
		[...el.options].some(o => o.value == v) ||
			el.append(h('option', { value: v }, 'UTC' + (m < 0 ? '−' : '+') + hh(m)));
		el.value = v;
	}
	function seed() {
		$('sy-man').value = new Date(Date.now() - new Date().getTimezoneOffset() * 6e4)
			.toISOString()
			.slice(0, 19);
	}
	const step = ms => {
		const a = Math.abs(ms);
		return a < 1e3 ? Math.round(a) + ' ms' : a < 6e4 ? (a / 1e3).toFixed(1) + ' s' : dur(a / 1e3);
	};
	function rTime(d) {
		const n = d.ntp || {};
		if (!filled) {
			$('sy-ntp').value = n.server || '';
			filled = 1;
		}
		tx(
			'sy-nh',
			!n.server
				? _('Network time is off now.')
				: n.synchronized
					? n.last_offset_ms != null
						? _('Synced {t} ago with {s}; last correction {c}.', { t: dur(n.last_sync_age_seconds), s: n.resolved || n.server, c: step(n.last_offset_ms) })
						: _('Synced {t} ago with {s}.', { t: dur(n.last_sync_age_seconds), s: n.resolved || n.server })
					: n.error
						? _('Not synced yet: {e}.', { e: n.error })
						: _('Not synced yet: waiting for the first reply.')
					);
		tick();
	}
	function tick() {
		if (route == 'overview' || route == 'audio') sirUI();
		/* countdowns between reads: the switch wait, the triggered light */
		if (S.x && (route == 'overview' || route == 'image')) {
			lights();
			route == 'image' && rCount();
		}
		if (route != 'system') return;
		const now = new Date();
		tx('sy-dev', hm(now));
		if (!S.t) return;
		const c = new Date(S.t.epoch * 1e3 + age('t')),
			df = Math.round(Math.abs(c - now) / 1e3);
		tx('sy-cam', c.toLocaleString(LC, { dateStyle: 'medium', timeStyle: 'medium' }));
		$('sy-match').replaceChildren(df < 3 ? pill('ok', _('matches')) : pill('warn', _('differs by {t}', { t: dur(df) })));
	}
	const putT = json => api(J + 'time', { method: 'PUT', json, timeout: 15e3 }),
		putZ = () =>
			api(J + 'timezone', { method: 'PUT', json: { gmt_tz: $('sy-tz').value }, timeout: 15e3 }).then(() => {
				tzd = 0;
				tzAt = Date.now() + 7e4;
			});
	/* "gateway", an IPv4 address with an optional port, or empty (off). */
	const ntpOk = v =>
		v == '' ||
		v == 'gateway' ||
		((m => !!m && m.slice(1, 5).every(x => x < 256) && (!m[5] || (m[5] > 0 && m[5] < 65536)))(
			/^(\d{1,3})\.(\d{1,3})\.(\d{1,3})\.(\d{1,3})(?::(\d{1,5}))?$/.exec(v)
		) &&
			v != '0.0.0.0');
	$('sy-save').onclick = act(async () => {
		const n = $('sy-ntp').value.trim();
		if (!ntpOk(n)) return bad($('sy-ntp'), CODES.invalid_ntp);
		let did = 0;
		if (n != (S.t?.ntp?.server || '')) {
			await putT({ ntp: n });
			did = 1;
		}
		if (tzd) {
			await putZ();
			did = 1;
		}
		toast(did ? _('Saved') : _('Nothing to save'));
		filled = 0;
		await Promise.all([clock(), joan()]);
	});
	$('sy-now').onclick = act(async () => {
		await putT({ epoch: String((Date.now() / 1e3) | 0) });
		await putZ();
		toast(_('Clock and time zone set from this device'));
		await clock();
	});
	$('sy-set').onclick = act(async () => {
		const v = $('sy-man').value,
			t = new Date(v).getTime();
		if (!v || !isFinite(t)) no(_('Choose a date and time first.'));
		await putT({ epoch: String((t / 1e3) | 0) });
		toast(_('Camera clock set'));
		await clock();
	}, $('sy-man'));
	/* Restart now: refused during an update or a trial; afterwards the offline banner waits for it. */
	const RESTART_NO = {
		update: _('An update is being installed. Try again when it has finished.'),
		'wifi-trial': _('A new Wi-Fi network is being tested. Keep it or go back first.'),
		'boot-trial': _('A new release is still being tried out after an update. Try again in a few minutes.'),
		'no-runtime': _('A platform update stopped half way, and the camera would not come back from a restart. Look at it over SSH first (see docs/RECOVERY.md).'),
		starting: _('The camera is still checking its start, and a restart now would count against it. Try again in about 4 minutes.')
	};
	$('sy-rs').onclick = () =>
		fwStopped ||
		sheet({
			title: _('Restart the camera?'),
			text: _('It is offline for about 90 seconds. Your NVR reconnects by itself and this page comes back on its own.'),
			ok: _('Restart now'),
			danger: 1,
			icon: 'system',
			run: async () => {
				fwStopped && no(_('Locked: an update stopped. Look at the camera over SSH first.'));
				try {
					await api(J + 'system/restart', { method: 'POST', json: {}, quiet: 1, timeout: 15e3 });
				} catch (x) {
					if (x.st == 409) no(RESTART_NO[x.d?.reason] || _('The camera can’t restart right now.'));
					if (x.kind != 'off') throw x;
				}
				holdOff = Date.now() + 8e3; // it answers for a moment before it goes down
				goOff(_('Restarting: this takes about 90 seconds, and this page comes back on its own.'));
			}
		});
	/* Reset to defaults: typing the camera's name confirms it. The camera checks the name too. */
	$('sy-rd').onclick = () => {
		if (fwStopped) return;
		const i = h('input', { id: 'rd-name', class: 'raw', autocomplete: 'off', autocapitalize: 'off', spellcheck: 'false' }),
			L = (t, a) => h('div', { class: 'f' }, h('span', { class: 'cap', text: t }), h('ul', { class: 'rl' }, ...a.map(x => h('li', { text: x }))));
		sheet({
			title: _('Reset {n} to defaults?', { n: name() }),
			text: _('The camera restarts and is back in about 90 seconds. Your NVR stops receiving video until you give it the new stream password.'),
			body: h(
				'div',
				{ class: 'stack s' },
				L(_('Erased'), [_('Picture, night, light and siren settings, and the microphone’s mute'), _('Presets, patrol and privacy settings'), _('SSH keys'), _('Scheduled restart'), _('Both passwords, back to initial values'), _('A fixed address: it goes back to automatic')]),
				L(_('Kept'), [S.q?.wifi?.ssid ? _('Wi-Fi: {ssid}', { ssid: S.q.wifi.ssid }) : _('Wi-Fi: the current network'), _('Local name and allowed networks'), _('Time zone and network time'), _('Mirror, frame rates, bitrates and whether audio is on'), _('The installed firmware')]),
				fld(_('Type {n} to confirm', { n: name() }), i)
			),
			ok: _('Reset to defaults'),
			danger: 1,
			focus: i,
			run: async () => {
				const v = i.value.trim();
				fwStopped && no(_('Locked: an update stopped. Look at the camera over SSH first.'));
				if (v != name()) no(_('Type {n} exactly.', { n: name() }));
				try {
					await api(J + 'system/reset', { method: 'POST', json: { confirm: v }, quiet: 1, timeout: 15e3 });
				} catch (x) {
					if (x.st == 409) no(RESTART_NO[x.d?.reason] || _('The camera can’t reset right now.'));
					if (x.kind != 'off') throw x;
				}
				holdOff = Date.now() + 8e3;
				goOff(_('Resetting: this takes about 90 seconds. Sign in again with the initial password.'));
			}
		});
	};
	/* Scheduled restart: daily, in the camera's time zone (UTC when it has none). */
	function restartNext(t) {
		const m = camZone() ? S.x.utc_offset_min : 0,
			now = (S.t ? S.t.epoch * 1e3 + age('t') : Date.now()) + m * 6e4,
			d = new Date(now),
			wait = (mins(t) - d.getUTCHours() * 60 - d.getUTCMinutes() + 1440) % 1440 || 1440,
			at = new Date(now + wait * 6e4);
		return (
			at.toLocaleDateString(LC, { weekday: 'short', day: 'numeric', month: 'short', timeZone: 'UTC' }) +
			', ' +
			t
		);
	}
	function rRestart(d) {
		const b = $('sy-rss'),
			i = $('sy-rst');
		busy(b) || sw(b, d.enabled);
		$('sy-rsf').hidden = !d.enabled;
		d.time && setIn(i, d.time);
		const unset = S.x?.live?.clock_ok === false;
		tx('sy-rsn', !d.enabled ? _('Off') : unset ? _('Once a day, counted from the last start') : _('Next: {t}', { t: restartNext(d.time) }));
		tx(
			'sy-rsz',
			unset
				? _('Nothing has confirmed the clock since the camera started, so it restarts 24 hours after the last start, until network time or you set it.')
				: camZone()
					? _('Every day, in the camera’s time zone ({z}).', { z: utc(S.x.utc_offset_min) })
					: _('Every day, in UTC: the camera has no time zone set yet.')
		);
	}
	const putRestart = async time => {
		const d = await api(J + 'system/restart-schedule', { method: 'PUT', json: { time }, timeout: 2e4 });
		S.y = d;
		S['y@'] = Date.now();
		rRestart(d);
		return d;
	};
	$('sy-rss').onclick = e => {
		const b = e.currentTarget,
			on = b.getAttribute('aria-checked') != 'true';
		run(b, async () => {
			const d = await putRestart(on ? $('sy-rst').value || '04:00' : '');
			sw(b, d.enabled);
			toast(d.enabled ? _('Restarts every day at {t}', { t: d.time }) : _('Scheduled restart off'));
		});
	};
	$('sy-rst').addEventListener('input', e => (e.target.dataset.d = 1));
	$('sy-rst').onchange = e => {
		const i = e.target;
		if (!i.value) return bad(i, _('Enter a time.'));
		run(
			i,
			async () => {
				await putRestart(i.value);
				delete i.dataset.d;
				toast(_('Restarts every day at {t}', { t: i.value }));
			},
			i
		);
	};
	function pwSheet() {
		const o = pwf(_('Current password'), 'current-password'),
			n = pwf(_('New password'), 'new-password'),
			r = pwf(_('Repeat the new password'), 'new-password');
		sheet({
			title: _('Change the admin password'),
			text: _('Used for this page and for SSH. You sign in again afterwards.'),
			icon: 'lock',
			ok: _('Change password'),
			focus: o.i,
			body: [h('input', { autocomplete: 'username', value: user, hidden: true }), o.f, n.f, r.f],
			run: async () => {
				const v = n.i.value;
				if (!o.i.value) no(_('Enter the current password.'));
				if (!v) no(_('Enter a new password.'));
				if (new TextEncoder().encode(v).length > 128) no(_('Use at most 128 characters.'));
				if (/[\x00-\x1f\x7f"\\]/.test(v))
					no(_('Passwords cannot contain quotes, backslashes or control characters.'));
				if (v != r.i.value) no(_('The new passwords do not match.'));
				await api(J + 'setup/password', {
					method: 'POST',
					json: { old_password: o.i.value, new_password: n.i.value },
					timeout: 2e4
				});
				$('sheet').close();
				out(_('Password changed. Sign in with the new password.'));
			}
		});
	}
	$('sy-pwa').onclick = $('sy-syncb').onclick = pwSheet;
	/* The stream account's password (RTSP, ONVIF, Home Assistant): 1-128 printable
	   characters without spaces at the ends. NVRs must be updated afterwards. */
	function streamPwSheet() {
		const n = pwf(_('New stream password'), 'new-password'),
			r = pwf(_('Repeat the new password'), 'new-password');
		sheet({
			title: _('Change the stream password'),
			text: _('Your NVR and Home Assistant disconnect until you update the password there too. The admin password stays the same.'),
			icon: 'lock',
			ok: _('Change password'),
			focus: n.i,
			body: [
				n.f,
				h('small', {
					text: _('Letters, digits and - . _ ~ need no escaping in NVR addresses.')
				}),
				r.f
			],
			run: async () => {
				const v = n.i.value;
				if (!v) no(_('Enter a new password.'));
				if (new TextEncoder().encode(v).length > 128) no(_('Use at most 128 characters.'));
				if (/[\x00-\x1f\x7f]/.test(v)) no(_('Passwords cannot contain control characters.'));
				if (v != v.trim()) no(_('Remove the spaces at the start or end.'));
				if (v != r.i.value) no(_('The new passwords do not match.'));
				await api(M + 'config', { method: 'POST', json: { password: v } });
				hidePw();
				toast(_('Stream password changed. Update it in your NVR.'));
				config().catch(noop);
			}
		});
	}
	$('sy-pwm').onclick = streamPwSheet;
	/* SSH keys: the blob is base64; its SHA-256, base64 without padding, is what ssh-keygen -l prints. */
	const unb64 = s => Uint8Array.from(atob(s), c => c.charCodeAt(0));
	async function fingerprint(blob) {
		try {
			const d = new Uint8Array(await crypto.subtle.digest('SHA-256', unb64(blob)));
			return 'SHA256:' + btoa(String.fromCharCode(...d)).replace(/=+$/, '');
		} catch {
			return '…' + blob.slice(-12);
		}
	}
	/* An ed25519 public key: "ssh-ed25519 <base64 of len 11, 'ssh-ed25519', len 32, 32 bytes> [comment]". */
	function edKey(v) {
		try {
			const k = unb64(v.split(' ')[1] || '');
			return (
				k.length == 51 &&
				k[3] == 11 &&
				k[18] == 32 &&
				new TextDecoder().decode(k.subarray(4, 15)) == 'ssh-ed25519'
			);
		} catch {
			return false;
		}
	}
	/* The list is raw text, one key per line; a key's id is its 1-based line, up to 8 keys. */
	const keyLines = d =>
		String(d?.authorized_keys || '')
			.split('\n')
			.filter(l => l.trim())
			.map(l => l.trim().split(/\s+/));
	function rKeys(d) {
		const it = keyLines(d).map(([t, b, ...c]) => {
			const nm = c.join(' ') || t,
				fp = h('small', { class: 'mono fp', text: t.slice(4) });
			fingerprint(b || '').then(f => (fp.textContent = t.slice(4) + ' · ' + f));
			return h(
				'li',
				null,
				ico('key'),
				h('span', null, h('b', { text: nm }), fp),
				h(
					'button',
					{ class: 'btn dz', 'aria-label': _('Remove the key for {n}', { n: nm }), onclick: () => rmKey(b, nm) },
					_('Remove…')
				)
			);
		});
		$('sy-keys').replaceChildren(
			...(it.length ? it : [h('li', { class: 'none', text: _('No keys. SSH uses the admin password.') })])
		);
	}
	const rmKey = (blob, nm) =>
		sheet({
			title: _('Remove the key for {nm}?', { nm }),
			text: _('SSH from that computer then needs the admin password or another key.'),
			ok: _('Remove'),
			danger: 1,
			icon: 'lock',
			run: async () => {
				/* Line numbers shift when keys change: find this key's line in a fresh list. */
				const i = keyLines(await keys()).findIndex(k => k[1] == blob);
				if (i >= 0)
					await api(J + 'ssh/authorized-keys', {
						method: 'DELETE',
						json: { id: String(i + 1) },
						timeout: 2e4
					});
				toast(_('SSH key removed'));
				await keys();
			}
		});
	$('sy-add').onsubmit = e => {
		e.preventDefault();
		const t = $('sy-key'),
			v = t.value.trim().replace(/\s+/g, ' ');
		if (!v.startsWith('ssh-ed25519 ') || v.split('ssh-ed25519 ').length > 2)
			return bad(t, _('Paste one key starting with ssh-ed25519.'));
		if (!edKey(v)) return bad(t, _('That key is incomplete: copy the whole line from your .pub file.'));
		const L = keyLines(S.k),
			dup = L.some(k => k[1] == v.split(' ')[1]),
			FULL = _('The camera keeps at most 8 keys. Remove one first.');
		if (!dup && L.length >= 8) return bad(t, FULL);
		run(
			sb(e),
			async () => {
				try {
					await api(J + 'ssh/authorized-keys', { method: 'POST', body: v, type: 'text/plain', timeout: 2e4 });
				} catch (x) {
					if (x.st == 502) no(FULL); // the list filled up meanwhile
					throw x;
				}
				t.value = '';
				toast(dup ? _('That key is already on the camera') : _('SSH key added'));
				await keys();
			},
			t
		);
	};
	/* Each package opens a short form once a file is chosen, or while there is something to say. */
	function upShow(box, file, nm, st, m) {
		const f = $(file).files[0];
		tx(nm, f ? f.name : '');
		$(box).hidden = !f && !st && !m;
	}
	$('sy-file').onchange = () => {
		fw = '';
		fwUI(0);
	};
	$('sy-mfile').onchange = () => {
		mfw = '';
		mfUI(0);
	};
	/* Upload stages: 0 idle, 1 uploading (can cancel), 2 checking, 3 staged, 4 installing. */
	function fwUI(st, m) {
		$('sy-prog').hidden = st != 1;
		tx('sy-ust', m || '');
		$('sy-apply').disabled = !fw || installing;
		upShow('sy-upb', 'sy-file', 'sy-fn', st, m);
	}
	/* o: where it goes and what shows it; only the platform's goes through the relay's slots. */
	function upload(f, o) {
		return (o.relay ? take() : Promise.resolve()).then(
			() =>
				new Promise((ok, no) => {
					const x = (upx = new XMLHttpRequest());
					x.onloadend = () => {
						upx = null;
						o.relay && give();
					};
					x.onabort = () => no(new Fail('', 'cancel'));
					x.open('POST', o.url);
					[
						['X-Requested-With', 'jipc-ui'],
						['X-CSRF-Token', csrf],
						['Content-Type', 'application/octet-stream'],
						['Accept', 'application/json']
					].forEach(([k, v]) => x.setRequestHeader(k, v));
					x.timeout = 18e4;
					x.upload.onprogress = e => {
						if (e.lengthComputable) {
							$(o.pb).value = (e.loaded / e.total) * 100;
							tx(o.pn, _('{a} of {b}', { a: bytes(e.loaded), b: bytes(e.total) }));
						}
					};
					x.upload.onload = o.checking;
					x.onload = () => {
						let d = null;
						try {
							d = JSON.parse(x.responseText);
						} catch {}
						if (x.status > 199 && x.status < 300) return ok(d);
						if (x.status == 401) {
							out(_('Your session ended. Sign in again.'));
							return no(new Fail('', 'auth'));
						}
						const er = d?.error;
						no(
							new Fail(
								x.status == 502 && o.refused
									? o.refused
									: say(x.status, er?.code, typeof er == 'string' ? er : er?.message || '')
							)
						);
					};
					x.onerror = x.ontimeout = () => no(new Fail(_('The upload stopped: the camera is not answering.')));
					x.send(f);
				})
		);
	}
	$('sy-up').onsubmit = e => {
		e.preventDefault();
		const f = $('sy-file').files[0];
		if (!f) return err($('sy-file'), _('Choose the package file first.'));
		if (f.size > 2097344) return err($('sy-file'), _('Too large for a platform package (2 MB).'));
		fw = '';
		tx('sy-pt', _('Uploading {f}', { f: f.name }));
		fwUI(1);
		run(
			$('sy-stage'),
			async () => {
				try {
					const d = await upload(f, {
						relay: 1,
						url: J + 'update',
						pb: 'sy-pb',
						pn: 'sy-pn',
						checking: () => fwUI(2, _('Checking the signature (up to a minute). Keep this page open.')),
						refused: _('The camera refused this package: it must be a signed, newer platform release for this camera.')
					});
					if (!d?.id) no(_('The camera did not stage the package. Try again.'));
					fw = d.id;
					fwV = d.version || '';
					fwUI(3, d.version ? _('Signature verified: platform {v}. Ready to install.', { v: d.version }) : _('Signature verified. Ready to install.'));
				} catch (x) {
					fwUI(0, x.kind == 'cancel' ? _('Upload cancelled.') : '');
					throw x;
				}
			},
			$('sy-file')
		);
	};
	$('sy-cancel').onclick = () => upx?.abort();
	/* A platform install runs on behind its answer. If it stops, nothing else would say so. */
	const FW_NOT_RESTARTED = _('Installed. The camera has not restarted, because a media update has not finished: look at it over SSH, then restart it.'),
		FW_STOPPED = _('The update stopped and the camera is still running. Before restarting it, look at it over SSH (see docs/RECOVERY.md).');
	/* What a status means. Only an install that got going can have left anything half done. */
	function fwReport(d) {
		const stopped = d.state == 'failed' && d.reason == 'upgrade',
			refused = d.state == 'failed' && !stopped,
			needed = d.restart == 'needed';
		if (!stopped && !refused && !needed) return 0;
		installing = 0;
		const still = stopped || (refused && fwWas); /* a refusal changes nothing: what stopped before is still there */
		fwStopped = +still;
		fwMsg = needed ? FW_NOT_RESTARTED : still ? FW_STOPPED : '';
		shell();
		$('sy-mapply').disabled = !mfw || fwStopped;
		$('sy-mstage').disabled = false;
		upx || fw || fwUI(0, needed ? FW_NOT_RESTARTED : stopped ? FW_STOPPED : _('The update did not start, and nothing was changed: the camera refused this package.'));
		return 1;
	}
	/* When this page opens too: the tab may have closed, or reloaded, while an install ran. */
	async function fwLast() {
		if (installing) return;
		const d = await api(J + 'update', { quiet: 1 });
		if (d.state == 'failed' && d.reason != 'upgrade') return; /* old news, and nothing changed */
		if (fwReport(d)) return;
		if (fwMsg && d.state == 'idle') {
			/* Repaired and restarted, with this page open. */
			fwStopped = 0;
			fwMsg = '';
			shell();
			fwWas = 0;
			$('sy-mapply').disabled = !mfw;
			upx || fw || fwUI(0);
		}
		if (d.state == 'running' || d.state == 'launching') {
			installing = 1;
			instAt ||= Date.now();
			$('sy-mapply').disabled = $('sy-mstage').disabled = true;
			fwUI(4, _('Installing. The camera restarts; this page reconnects by itself.'));
			fwWatch();
		}
	}
	async function fwWatch() {
		if (fwW) return; /* the page opening on System asks twice */
		fwW = 1;
		let lost = 0;
		for (let i = 0; i < 240 || lost; i++) {
			await new Promise(z => setTimeout(z, 2e3));
			let d = null;
			try {
				d = await api(J + 'update', { quiet: 1 });
			} catch (x) {
				if (x.kind == 'auth') return location.reload(); /* the restart ended this session: sign in on the new page */
			}
			/* No answer says nothing: Wi-Fi, or the restart. What it says afterwards decides. */
			if (!d) {
				lost++;
				if (!instLate && Date.now() - instAt > 3e5) {
					instLate = 1;
					shell();
					fwUI(4, _('It has not come back after 5 minutes. Do not cut its power yet: look at it over SSH first (see docs/RECOVERY.md).'));
				}
				continue;
			}
			if (instLate) {
				instLate = 0;
				shell();
			}
			if (lost && d.state == 'idle') return location.reload(); /* the status went with the restart: the buttons and the versions are a new page's */
			lost = 0;
			if (fwReport(d)) return void (fwW = 0);
		}
		fwW = 0;
		installing = 0;
		fwStopped = 1;
		fwMsg = _('Not sure how this went: the camera still answers after 8 minutes and has not restarted. Before restarting it, look at it over SSH (see docs/RECOVERY.md).');
		shell();
		$('sy-mapply').disabled = true;
		$('sy-mstage').disabled = false;
		fwUI(0, fwMsg);
	}
	$('sy-apply').onclick = () =>
		fw &&
		sheet({
			title: _('Install and restart?'),
			text: _('The camera installs the verified package and restarts; it is offline for about 90 seconds. Do not cut its power meanwhile. If it does not come back, recovery is over SSH.'),
			body: fwV && h('p', { class: 'mono', text: _('Package: {v}', { v: fwV }) }),
			ok: _('Install and restart'),
			danger: 1,
			icon: 'system',
			run: async () => {
				try {
					installing = 1;
					instAt = Date.now();
					$('sy-mapply').disabled = $('sy-mstage').disabled = true;
					await api(J + 'update', { method: 'PUT', json: { id: fw }, timeout: 6e4, quiet: 1 });
				} catch (x) {
					if (x.kind != 'off') {
						installing = 0;
						instAt = 0;
						shell();
						$('sy-mapply').disabled = !mfw || !!fwStopped;
						$('sy-mstage').disabled = false;
						if (x.st == 409 && x.d?.reason == 'wifi-trial') throw new Fail(RESTART_NO['wifi-trial']);
						throw x;
					}
				}
				fwWas = fwStopped;
				fwStopped = 0;
				fwMsg = '';
				fw = '';
				$('sy-file').value = '';
				shell();
				fwUI(4, _('Installing. The camera restarts; this page reconnects by itself.'));
				toast(_('Installing. The camera restarts now.'));
				/* Not awaited: the dialog closes, and it does not look as if Cancel could stop this. */
				fwWatch();
			}
		});
	/* A media package: the same three steps. A failure before the flash leaves the camera up
	   and says why; otherwise it restarts, for longer than a platform update takes. */
	let mfw = '';
	function mfUI(st, m) {
		$('sy-mprog').hidden = st != 1;
		tx('sy-must', m || '');
		$('sy-mapply').disabled = !mfw || installing || fwStopped;
		upShow('sy-mupb', 'sy-mfile', 'sy-mfn', st, m);
	}
	$('sy-mup').onsubmit = e => {
		e.preventDefault();
		const f = $('sy-mfile').files[0];
		if (!f) return err($('sy-mfile'), _('Choose the package file first.'));
		if (f.size > 34e5) return err($('sy-mfile'), _('Too large for a media package.'));
		mfw = '';
		tx('sy-mpt', _('Uploading {f}', { f: f.name }));
		mfUI(1);
		run(
			$('sy-mstage'),
			async () => {
				try {
					const d = await upload(f, {
						url: M + 'update/media',
						pb: 'sy-mpb',
						pn: 'sy-mpn',
						checking: () => mfUI(2, _('Checking the signature. Keep this page open.'))
					});
					if (!d?.release) no(_('The camera did not keep the package. Try again.'));
					mfw = d.release;
					mfUI(3, _('Signature verified: release {release}. Ready to install.', { release: d.release }));
				} catch (x) {
					mfUI(0, x.kind == 'cancel' ? _('Upload cancelled.') : '');
					throw x;
				}
			},
			$('sy-mfile')
		);
	};
	$('sy-mcancel').onclick = () => upx?.abort();
	$('sy-mapply').onclick = () =>
		mfw &&
		sheet({
			title: _('Install and restart?'),
			text: _('The camera writes the new media firmware and restarts. Video and this page are away for about 2 minutes, and your NVR reconnects by itself. Do not cut its power meanwhile. If it does not come back, recovery is over SSH.'),
			body: h('p', { class: 'mono', text: _('Package: {v}', { v: mfw }) }),
			ok: _('Install and restart'),
			danger: 1,
			icon: 'system',
			run: async () => {
				await api(M + 'update/media/apply', { method: 'POST', json: { release: mfw }, timeout: 3e4 });
				mfw = '';
				installing = 1;
				instAt = Date.now();
				$('sy-apply').disabled = $('sy-stage').disabled = true;
				$('sy-mfile').value = '';
				holdOff = Date.now() + 8e3;
				reloadOn = 1;
				shell();
				mfUI(4, _('Installing. The camera restarts; this page reconnects by itself.'));
				toast(_('Installing. The camera restarts soon.'));
				/* A flash that fails stops the daemon first, so no message can come from it. */
				setTimeout(() => {
					if (!reloadOn) return;
					instLate = 1;
					shell();
					mfUI(4, _('It has not come back after 5 minutes. Do not cut its power yet: look at it over SSH first (see docs/RECOVERY.md).'));
				}, 3e5);
				/* It says so if it stopped short of the flash; once it goes away, it has not. */
				for (let i = 0; i < 36; i++) { /* stopping the media daemon can take a minute before it refuses */
					await new Promise(z => setTimeout(z, 2500));
					const d = await api(M + 'update/media', { quiet: 1 }).catch(() => null);
					if (!d) return;
					if (d.error) {
						reloadOn = 0;
						installing = 0;
						instAt = 0;
						shell();
						$('sy-apply').disabled = !fw;
						$('sy-stage').disabled = false;
						return mfUI(0, _('Not installed, nothing changed: {e}', { e: d.error }));
					}
				}
			}
		});
	/* A tooltip: a real button beside what it explains, and a note under that line. Tap or click
	   pins it; a mouse also opens it on hover; Escape closes it and returns to the button. */
	let tipN = 0,
		tipB = null,
		tipPin = 0,
		tipW;
	function tip(l, t, d) {
		const id = 'tp' + ++tipN,
			b = h('button', { type: 'button', class: 'tip', 'aria-expanded': 'false', 'aria-controls': id, 'aria-label': _('About: {x}', { x: l }) }, ico('info')),
			n = h('span', { class: 'tipb', id, role: 'note', hidden: true, text: t });
		d && $(d)?.setAttribute('aria-describedby', id);
		return h('span', { class: 'tipw' }, b, n);
	}
	function tipSet(b, pin) {
		clearTimeout(tipW);
		if (tipB && tipB != b) {
			tipB.setAttribute('aria-expanded', 'false');
			tipB.nextElementSibling.hidden = true;
		}
		tipB = b;
		tipPin = pin;
		if (b) {
			b.setAttribute('aria-expanded', 'true');
			b.nextElementSibling.hidden = false;
		}
	}
	document.addEventListener('click', e => {
		const b = e.target.closest('.tip');
		if (b) tipB == b && tipPin ? tipSet(null) : tipSet(b, 1);
		else if (tipB && !e.target.closest('.tipb')) tipSet(null);
	});
	document.addEventListener('pointerover', e => {
		if (e.pointerType != 'mouse' || tipPin) return;
		const w = e.target.closest?.('.tipw');
		clearTimeout(tipW);
		tipW = setTimeout(() => (w ? tipSet(w.firstChild, 0) : tipB && tipSet(null)), 120);
	});
	document.addEventListener('keydown', e => {
		if (e.key == 'Escape' && tipB) {
			const b = tipB;
			tipSet(null);
			b.focus();
		}
	});
	/* One place on every page for what could hurt, most dangerous first. */
	function attn() {
		const live = S.x?.live,
			P0 = S.m?.platform,
			X = S.x,
			on = sirOn(),
			fl = left(live?.strobe_left_s || 0),
			ssh = `ssh ${user}@${S.q?.wifi?.address || location.hostname}`,
			sc = (i, b) => $(i).dataset.k != ssh && (($(i).dataset.k = ssh), $(i).replaceChildren(cf(ssh, 'SSH')));
		$('at-stop').hidden = !fwMsg;
		if (fwMsg) {
			tx('at-stopt', fwStopped ? _('Update stopped: do not restart the camera or cut its power') : _('An update needs a look'));
			tx('at-stopp', fwMsg + (fwStopped ? ' ' + _('Restart and reset are locked until then.') : ''));
			sc('at-ssh');
		}
		$('sy-rs').disabled = $('sy-rd').disabled = !!fwStopped;
		$('sy-lock').hidden = !fwStopped;
		$('sy-rss').disabled = !!fwStopped && $('sy-rss').getAttribute('aria-checked') != 'true';
		$('at-late').hidden = !instLate;
		instLate && sc('at-ssh2');
		$('at-inst').hidden = !installing || !!fwMsg || !!instLate;
		if (installing) {
			tx('at-instp', reloadOn ? _('The camera is writing the update and then restarts. Video and this page are away for about 2 minutes. This page reconnects by itself.') : _('The camera is writing the update and then restarts. It is away for about 90 seconds. This page reconnects by itself.'));
			tx('at-el', _('{t} so far', { t: dur((Date.now() - instAt) / 1e3) }));
		}
		const rs = !!off && !!restarting && Date.now() - restAt < 18e4;
		$('at-rest').hidden = !rs;
		if (rs) {
			tx('at-restt', _('{n} isn’t answering', { n: label() }));
			tx('at-restp', restarting);
		}
		$('banner').hidden = !off || rs || (!!installing && !instLate);
		$$('.lang').forEach(e => (e.disabled = !!(installing || off)));
		off && tx('bn-t', _('{n} isn’t answering', { n: label() }));
		$('at-sir').hidden = !on && !fl;
		(on || fl) && tx('at-sirt', on ? _('Siren sounding: {s} s left', { s: sirLeft() }) : _('The white light is flashing: {s} s left.', { s: fl }));
		const trial = W && !W.end && route != 'network';
		$('at-try').hidden = !trial;
		trial && tx('at-tryt', _('Testing {ssid}: {t} left to decide', { ssid: W.ssid, t: mmss(Math.ceil(Math.max(0, W.deadline - Date.now()) / 1e3)) }));
		$('at-priv').hidden = !(P0 ? P0.privacy : live?.privacy) || !!off;
		$('at-clock').hidden = !(live?.clock_ok === false && (X.night.mode == 'schedule' || X.white_light.schedule.enabled || X.privacy?.schedule.enabled || X.overlay?.enabled)) || !!off;
	}
	function theme(t) {
		const r = document.documentElement;
		t == 'light' || t == 'dark' ? (r.dataset.theme = t) : (delete r.dataset.theme, (t = 'auto'));
		ls.set(THEME, t == 'auto' ? null : t);
		$$('.theme').forEach(s => pick(s, t));
	}
	$$('#side a,#tabs a,.menu a').forEach(a => {
		const i = ico(a.hash.slice(1));
		a.closest('#tabs') ? a.prepend(h('span', null, i)) : a.prepend(i);
		a.closest('.menu') && a.append(ico('chev'));
	});
	$$('svg').forEach(s => {
		s.setAttribute('aria-hidden', 'true');
		s.hasAttribute('viewBox') || s.setAttribute('viewBox', '0 0 24 24');
	});
	$$('.seg').forEach(s => pick(s));
	$$('.sw').forEach(b => {
		b.setAttribute('role', 'switch');
		sw(b, 0);
		b.append(h('span'));
	});
	$$('i.tip').forEach(x => x.replaceWith(tip(x.dataset.l, x.dataset.t, x.dataset.d)));
	$$('.lang').forEach(el => {
		const cur = document.documentElement.lang;
		el.replaceChildren(...Object.entries(LANGS).map(([k, v]) => h('option', { value: k, selected: k == cur, lang: k }, v)));
		el.onchange = () => {
			ls.set(LANG_KEY, el.value);
			location.reload();
		};
	});
	tx('si-cert', _('The camera makes its own certificate, so the first visit may warn. Check the address is {host}.', { host: location.host }));
	$$('.reveal').forEach(b => {
		b.setAttribute('aria-label', _('Show password'));
		b.setAttribute('aria-pressed', 'false');
	});
	$$('.theme').forEach(s => seg(s, theme));
	theme(ls.get(THEME));
	pick($('im-lens'), 0);
	pick($('pt-speed'), speed);
	pick($('pt-view'), 'live');
	$$('.raw,[type=password]').forEach(i => {
		i.autocomplete = i.autocomplete || 'off';
		i.autocapitalize = 'off';
		i.spellcheck = false;
	});
	$$('.host').forEach(e => (e.textContent = location.host));
	$$('.cam-name').forEach(e => (e.textContent = label()));
	setInterval(() => {
		tick();
		W && !W.end && wifi();
		if (!csrf || document.hidden) return;
		attn();
		if (fwMsg && !installing && age('f') > 3e4) {
			S['f@'] = Date.now();
			fwLast().catch(noop);
		}
		stShow();
		stTick();
		if (off) {
			tx('bn-s', _('Retrying in {s} s.', { s: Math.max(0, Math.ceil((retry - Date.now()) / 1e3)) }));
			Date.now() >= retry && probe();
			return;
		}
		/* Media status, which the attention bar needs on every page: every 5 s, 10 s where nothing else
		   on the page uses it; 1 s while pan/tilt moves or the siren sounds; 2 s on Audio for the level meter. */
		const mv = fast > Date.now() || S.m?.platform?.moving;
		if (age('m') > (sirOn() || (mv && route == 'pantilt') ? 1e3 : route == 'audio' ? 2e3 : ['streams', 'network', 'settings'].includes(route) ? 1e4 : 5e3)) media().catch(noop);
		if (route == 'pantilt') {
			rAim();
			if (age('a') > (mv ? 1e3 : 5e3)) aim().catch(noop);
		}
		/* Controls: every 2 s on Image & night (the darkness meter), 5 s on Overview, 30 s elsewhere for the bar. */
		if (age('x') > (route == 'image' ? 2e3 : route == 'pantilt' ? 3e3 : route == 'overview' || route == 'audio' ? 5e3 : 3e4)) controls().catch(noop);
		/* A Wi-Fi trial: every 3 s while one runs; otherwise now and then on Network. */
		if (age('w') > (joining || (W && !W.end) ? 3e3 : route == 'network' ? 2e4 : 1 / 0)) wifiState().catch(wifiLost);
		if ((route == 'overview' || route == 'system') && age('j') > 6e4) joan().catch(noop);
		if (['overview', 'network', 'settings'].includes(route) && age('q') > 3e4) quality().catch(noop);
		if (age('ka') > 24e4) {
			S['ka@'] = Date.now();
			api(J + 'session', { quiet: 1 })
				.then(d => d?.csrf && (csrf = d.csrf))
				.catch(noop);
		}
	}, 1e3);
	(async () => {
		try {
			const d = await api(J + 'session', { login: 1, quiet: 1 });
			if (!d?.csrf) throw 0;
			csrf = d.csrf;
			S['ka@'] = Date.now();
			enter();
		} catch (e) {
			const o = e?.kind == 'off';
			showSignIn(o ? NA : '', o && 'crit');
		}
	})();
}
{
	const l = langOf();
	let began = 0;
	const start = pack => {
		if (began++) return; /* a pack that arrives after the wait is too late */
		PACK = pack || null;
		const x = PACK ? l : 'en';
		document.documentElement.lang = x;
		LC = (x != 'zh-CN' && (navigator.languages || []).find(y => y.startsWith(x.slice(0, 2)))) || x;
		if (PACK) PR = new Intl.PluralRules(LC);
		walk();
		main();
	};
	if (l == 'en') start();
	else {
		const e = document.createElement('script');
		e.src = '/ui/lang-' + l + '.js';
		e.onload = () => start(self.JIPC_PACK);
		e.onerror = () => start();
		setTimeout(() => start(), 6e3); /* the page must not wait for ever on a pack that hangs */
		document.head.append(e);
	}
}
