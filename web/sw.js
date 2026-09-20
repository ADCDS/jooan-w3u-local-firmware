'use strict';
const CACHE='jooan-ui-v2';
const STATIC=new Set(['/','/index.html','/styles.css','/app.js','/video-player.js','/manifest.webmanifest','/icon.svg']);
self.addEventListener('install',event=>event.waitUntil(caches.open(CACHE).then(cache=>cache.addAll([...STATIC])).then(()=>self.skipWaiting())));
self.addEventListener('activate',event=>event.waitUntil(caches.keys().then(keys=>Promise.all(keys.filter(key=>key!==CACHE).map(key=>caches.delete(key)))).then(()=>self.clients.claim())));
self.addEventListener('fetch',event=>{const url=new URL(event.request.url);if(event.request.method!=='GET'||url.origin!==self.location.origin||!STATIC.has(url.pathname)||url.pathname.startsWith('/api/')||url.pathname.startsWith('/ws/')||url.pathname.includes('audio'))return;event.respondWith(fetch(event.request,{cache:'no-store'}).then(response=>{if(response.ok)caches.open(CACHE).then(cache=>cache.put(event.request,response.clone()));return response;}).catch(()=>caches.match(event.request)));});
