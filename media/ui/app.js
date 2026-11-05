// SPDX-License-Identifier: MIT
// camera1 admin UI: vanilla JS, no build step, compact because it is embedded
// in the media daemon. Platform calls go through its /api/joan/ relay.
'use strict';(()=>{
const $=i=>document.getElementById(i),$$=(s,r=document)=>[...r.querySelectorAll(s)],noop=()=>{},J='/api/joan/v1/',M='/api/v1/';
const tx=(i,t)=>{$(i).textContent=t},no=m=>{throw new Fail(m)},bad=(el,m)=>{el.setAttribute('aria-invalid','true');err(el,m)},act=(f,at)=>e=>run(e.currentTarget,f,at);
function h(t,p,...k){const e=document.createElement(t);for(const a in p||{}){const v=p[a];if(v==null||v===false)continue;if(a=='text')e.textContent=v;else if(a[0]=='o'&&a[1]=='n')e.addEventListener(a.slice(2),v);else e.setAttribute(a,v===true?'':v)}e.append(...k.flat().filter(x=>x!=null&&x!==false));return e}
function ico(n){const s=document.createElementNS('http://www.w3.org/2000/svg','svg');s.innerHTML=`<use href="#i-${n}"/>`;s.setAttribute('viewBox','0 0 24 24');s.setAttribute('aria-hidden','true');return s}
const ls={get(k){try{return JSON.parse(localStorage.getItem(k))}catch(_){return null}},set(k,v){try{v==null?localStorage.removeItem(k):localStorage.setItem(k,JSON.stringify(v))}catch(_){}}};
const THEME='jipc-theme',WIFI='jipc-wifi';
let csrf='',ep=0,off=0,route='',user='admin',retry=0,W=null,joining=0,filled=0,tzd=0,pb=null,pt=0,pq=Promise.resolve(),pn=0,fast=0,lastP='',hs='',fw='',lens=0;
const S={},age=k=>Date.now()-(S[k+'@']||0);
const p2=n=>String(n).padStart(2,'0'),pl=(n,w)=>n+' '+w+(n==1?'':'s'),mi=n=>String(n).replace('-','−'),sg=n=>(n>0?'+':'')+mi(n),cap=s=>s?s[0].toUpperCase()+s.slice(1):'';
const hm=d=>d.toLocaleTimeString();
function dur(s){s=Math.max(0,Math.round(s));if(s<60)return s+' s';const m=s/60|0,H=m/60|0;return m<60?m+' min':H<48?H+' h'+(m%60?' '+m%60+' min':''):(H/24|0)+' d'+(H%24?' '+H%24+' h':'')}
function bytes(n){n=+n;if(!isFinite(n))return'Unknown';let i=0;while(n>=1e3&&i<4){n/=1e3;i++}return(i?n.toFixed(n<10?2:1):n)+' '+['bytes','kB','MB','GB','TB'][i]}
const sig=d=>d==null?'':d>=-50?'Excellent':d>=-60?'Good':d>=-70?'Fair':'Weak';
const pill=(t,x)=>h('span',{class:'pill '+t},ico(t),x);
let tt;function toast(m){const t=$('toast');t.textContent=m;t.classList.add('show');clearTimeout(tt);tt=setTimeout(()=>t.classList.remove('show'),3200)}
const host=e=>e.closest('.sl,.row,.f,.acts,dialog,form,.card')||e;
function clr(e){$$(':scope>.err',host(e)).forEach(x=>x.remove())}
function err(e,m){clr(e);host(e).append(h('p',{class:'err',role:'alert'},ico('crit'),h('span',{text:m})))}
const sb=e=>e.submitter||e.target.querySelector('button:not([type=button])'),busy=e=>e.getAttribute('aria-busy')=='true',OFF='Not sent: the camera is not answering.';
async function run(e,f,at){if(!e||busy(e))return;at=at||e;clr(at);e.setAttribute('aria-busy','true');e.setAttribute('aria-disabled','true');try{return await f()}catch(x){if(x.kind!='auth'||x.message)err(at,x.kind=='off'?OFF:x.message||'That did not work. Try again.')}finally{e.removeAttribute('aria-busy');e.removeAttribute('aria-disabled')}}
class Fail extends Error{constructor(m,kind,st,raw){super(m);Object.assign(this,{kind,st,raw})}}
const CODES={invalid_credentials:'That user name or password is not right.',rate_limited:'Too many tries. Wait a minute.',csrf_rejected:'Reload the page and try again.',invalid_ntp:'Use “gateway”, an IPv4 address (with an optional :port) or nothing.',invalid_time:'Pick a date between 2020 and 2100.',invalid_hostname:'Use 1–63 letters, digits or inner hyphens.',invalid_routes:'Only private networks can be added: 10.x, 172.16–31.x, 192.168.x or fd00::/8.',password_rejected:'The current password is wrong or the new one is not allowed.',integration_failed:'The camera could not finish that. Try again in a moment.'};
const MED=[[/homing/,'Pan/tilt is calibrating after start-up.'],[/already exists/,'Another preset already has that name.'],[/another talker/,'Someone else is talking through the camera.'],[/boot hook|muted until/,'The speaker stays muted until start-up finishes.'],[/not ready|stopping/,'The video service is starting. Try again shortly.'],[/[Ss]peaker/,'The camera’s speaker is not available right now.']];
function say(st,code,raw){if(CODES[code])return CODES[code];for(const[r,m]of MED)if(r.test(raw))return m;if(st==429)return CODES.rate_limited;if(raw&&raw.length<140&&!/[{<]/.test(raw))return cap(raw).replace(/\.?$/,'.');return st>=500?CODES.integration_failed:'The camera refused that request.'}
/* The media daemon relays at most two /api/joan/ requests at a time. */
let jn=0;const jq=[],take=()=>jn<2?(jn++,Promise.resolve()):new Promise(r=>jq.push(r)),give=()=>{const n=jq.shift();n?n():jn--};
async function req(p,o={}){const j=p.startsWith(J);j&&await take();const m=o.method||'GET',hd={'X-Requested-With':'jipc-ui',Accept:'application/json'};let b=o.body;if(m!='GET'&&csrf)hd['X-CSRF-Token']=csrf;if(o.json){hd['Content-Type']='application/json';b=JSON.stringify(o.json)}else if(o.type)hd['Content-Type']=o.type;const ac=new AbortController(),t=setTimeout(()=>ac.abort(),o.timeout||1e4);try{const r=await fetch(p,{method:m,headers:hd,body:b,signal:ac.signal,cache:'no-store'});return{r,d:r.status==204?null:(r.headers.get('content-type')||'').includes('json')?await r.json().catch(()=>null):await r.text()}}catch(_){return null}finally{clearTimeout(t);j&&give()}}
async function api(p,o={}){const e0=ep,x=await req(p,o);if(!x){o.quiet||goOff();throw new Fail('The camera is not answering.','off')}if(e0!=ep&&!o.login)throw new Fail('','auth');if(off)setOn();const{r,d}=x;if(r.ok)return d;const e=d&&d.error,code=e&&e.code||'',raw=typeof e=='string'?e:e&&e.message||(typeof d=='string'?d:'');
if(r.status==401&&!o.login){out('Your session ended. Sign in again.');throw new Fail('','auth')}
if(r.status==503&&/busy/.test(raw)&&!o.bz){await new Promise(z=>setTimeout(z,800));return api(p,{...o,bz:1})}
throw new Fail(say(r.status,code,raw),'http',r.status,raw)}
const ctl=(operation,value=0,value2=0,name)=>api(M+'control',{method:'POST',json:{operation,value,value2,name}});
function src(k,p,f,o){let q;return()=>q||(q=api(p,o).then(d=>{S[k]=d;S[k+'@']=Date.now();f(d);return d}).finally(()=>q=null))}
const media=src('m',M+'status',rMedia),config=src('c',M+'config',rConfig),joan=src('j',J+'status',rJoan),mdns=src('n',J+'network/mdns',rName),quality=src('q',J+'network/quality',rQuality,{timeout:2e4}),routes=src('r',J+'network/routes',rRoutes,{timeout:2e4}),keys=src('k',J+'ssh/authorized-keys',rKeys,{timeout:2e4}),clock=src('t',J+'time',rTime);
const guess=/^[\d.]+$|[:[]/.test(location.hostname)?'Camera':location.hostname.split('.')[0],name=()=>S.n?.hostname||guess,rel=()=>(/\/releases\/([0-9a-f]{8})/.exec(S.c?.libimp||'')||[])[1];
function goOff(){if(!csrf||off)return;off=1;retry=Date.now()+1e4;ptzEnd(1);talkStop();place();shell();ov()}
function setOn(){off=0;place();shell();ov();load(route)}
async function probe(){retry=Date.now()+1e4;const e0=ep,x=await req(J+'session',{timeout:6e3});if(e0!=ep||!x)return;if(x.r.status==401)return out('Your session ended. Sign in again.');if(x.d?.csrf)csrf=x.d.csrf;setOn()}
function place(){const pg=$('p-'+route),b=$('banner');b.hidden=!off;pg&&pg.querySelector('.ph').after(b);for(const c of $$('.page>*'))if(c!=b&&!c.classList.contains('ph'))c.inert=off&&!(W&&c.contains($('nw-count')))}
$('bn-try').onclick=e=>run(e.currentTarget,probe);
function note(m,t){const n=$('si-note');n.hidden=!m;n.className='banner '+(t||'info');n.replaceChildren(ico(t?'crit':'ok'),h('span',{text:m||''}))}
function showSignIn(m,t){$('app').hidden=true;$('signin').hidden=false;document.title=name()+' · Sign in';$('si-pass').value='';note(m,t);
req(J+'setup/status',{timeout:6e3}).then(x=>{const d=x?.r.ok&&x.d,f=$('si-first');f.hidden=!d?.default_password_warning;f.textContent=`Initial value in use: ${d?.temporary_user||'admin'} / admin.`})}
const NA='The camera is not answering. It may be restarting (about 90 s) or off the network.';
async function signIn(c){const e0=++ep;csrf='';let d;try{d=await api(J+'session',{method:'POST',json:c,login:1,quiet:1,timeout:15e3})}catch(x){if(x.kind=='off')no(NA);x.st==401&&$('si-pass').setAttribute('aria-invalid','true');throw x}if(e0!=ep)return;if(!d?.csrf)no('No session was started. Try again.');csrf=d.csrf;user=c.username;enter()}

$('si-form').onsubmit=e=>{e.preventDefault();const u=$('si-user').value.trim(),p=$('si-pass').value;if(!u||!p)return err($('si-pass'),'Enter the user name and the password.');run($('si-go'),()=>signIn({username:u,password:p}),$('si-pass'))};
function out(m){++ep;csrf='';off=0;ptzEnd(1);talkStop();place();showSignIn(m);$('si-h').focus()}
$$('.signout').forEach(b=>b.onclick=()=>run(b,async()=>{const e0=++ep;await req(J+'session',{method:'DELETE',timeout:5e3});e0==ep&&out('You are signed out.')}));
function enter(){$('signin').hidden=true;$('app').hidden=false;$$('.user').forEach(e=>e.textContent=user);W=ls.get(WIFI);if(W&&!(W.deadline>Date.now()))ls.set(WIFI,W=null);wifi();[config,mdns,media,joan].forEach(f=>f().catch(noop));route='';go(1)}
const TI={overview:'Overview',pantilt:'Pan/tilt',image:'Image & night',audio:'Audio',streams:'Streams & integrations',network:'Network',system:'System',settings:'Settings'},SUB=['streams','network','system'],wide=matchMedia('(min-width:900px)');
const title=()=>csrf&&(document.title=name()+' · '+TI[route]);
function go(f){let r=location.hash.slice(1);if(!TI[r])r='overview';if(r=='settings'&&wide.matches)history.replaceState(null,'','#'+(r='streams'));if(r!=route){talkStop();ptzEnd();filled=0}route=r;$$('.page').forEach(p=>p.hidden=p.id!='p-'+r);const tab=SUB.includes(r)?'settings':r;
for(const a of $$('#side a,#tabs a')){const k=a.hash.slice(1);(a.closest('#tabs')?k==tab:k==r)?a.setAttribute('aria-current','page'):a.removeAttribute('aria-current')}
$('top').classList.toggle('sub',SUB.includes(r));place();title();r=='system'&&seed();f&&$('p-'+r).querySelector('h1').focus();scrollTo(0,0);load(r)}
onhashchange=()=>csrf&&go(1);wide.onchange=()=>route=='settings'&&go();
const LD={overview:[media,joan,quality],pantilt:[media],image:[media,config],audio:[media,config],streams:[config,mdns,media],network:[[quality,'nw-now'],mdns,[routes,'nw-routes']],system:[joan,media,config,[clock,'sy-nh'],[keys,'sy-keys']],settings:[joan,media,quality]};
function load(r){for(const x of LD[r]||[]){const[f,at]=[].concat(x);f().catch(e=>e.kind=='http'&&at&&err($(at),e.message))}}
function pick(el,v){const bs=$$('button',el);let any;for(const b of bs){const on=b.value===String(v);any||=on;b.setAttribute('role','radio');b.setAttribute('aria-checked',on);b.tabIndex=on?0:-1}if(!any&&bs[0])bs[0].tabIndex=0}
function seg(el,f){el.addEventListener('click',e=>{const b=e.target.closest('button');b&&b.getAttribute('aria-checked')!='true'&&f(b.value)});el.addEventListener('keydown',e=>{const d={ArrowRight:1,ArrowDown:1,ArrowLeft:-1,ArrowUp:-1}[e.key],bs=$$('button',el),i=bs.indexOf(document.activeElement);if(!d||i<0)return;e.preventDefault();e.stopPropagation();const n=bs[(i+d+bs.length)%bs.length];n.focus();f(n.value)})}
const sw=(b,on)=>b.setAttribute('aria-checked',!!on),SL={};
function slider(id,label,min,max,v,fmt,set){const i=h('input',{id,type:'range',min,max,value:v}),o=h('output',{for:id,class:'mono',text:fmt(v)}),row=h('div',{class:'sl'},h('div',null,h('label',{for:id,text:label}),o),i);let t,b=0,nx=null;
const send=async()=>{if(b)return;b=1;while(nx!=null){const x=nx;nx=null;clr(i);try{await set(x)}catch(e){e.kind!='auth'&&err(i,e.kind=='off'?OFF:e.message)}}b=0};
i.oninput=()=>{o.textContent=fmt(+i.value);clearTimeout(t);t=setTimeout(()=>{nx=+i.value;send()},250)};SL[id]={row,i,o,fmt,idle:()=>!b&&nx==null&&document.activeElement!=i};return row}
function setSl(id,v,force){const s=SL[id];s.row.hidden=v==null;if(v!=null&&(force||s.idle())){s.i.value=v;s.o.textContent=s.fmt(v)}}
function cf(t,label){const c=h('code',{text:t}),b=h('button',{'aria-label':'Copy '+label},ico('copy'),h('span',{text:'Copy'}));b.onclick=()=>copy(t,b,c);return h('div',{class:'cf'},c,b)}
async function copy(t,b,el){let ok;try{await navigator.clipboard.writeText(t);ok=1}catch(_){getSelection().selectAllChildren(el)}const sp=b.querySelector('span');sp.textContent=ok?'Copied':'Selected';b.classList.toggle('done',!!ok);clearTimeout(b.t);b.t=setTimeout(()=>{sp.textContent='Copy';b.classList.remove('done')},2e3)}
let uid=0;function pwf(label,ac){const i=h('input',{type:'password',autocomplete:ac,id:'p'+ ++uid,maxlength:128});return{i,f:fld(label,i)}}
document.addEventListener('click',e=>{const b=e.target.closest('.reveal');if(!b)return;const i=b.previousElementSibling,on=i.type=='password';i.type=on?'text':'password';b.setAttribute('aria-pressed',on);b.setAttribute('aria-label',(on?'Hide':'Show')+' password')});
document.addEventListener('input',e=>{const t=e.target;if(t.hasAttribute?.('aria-invalid')){t.removeAttribute('aria-invalid');clr(t)}});
function sheet(o){const d=$('sheet'),no=h('button',{type:'button',class:'btn',onclick:()=>d.close()},'Cancel'),ok=h('button',{class:'btn '+(o.danger?'dzf':'pri')},o.ok),f=h('form',{class:'stack s',novalidate:true},o.body,h('div',{class:'acts'},no,ok),o.extra);
f.onsubmit=e=>{e.preventDefault();run(ok,async()=>{await o.run();d.open&&d.close()},f)};
d.replaceChildren(h('div',{class:'sh-h'},h('span',{class:'ico '+(o.danger?'crit':'wait')},ico(o.icon||'warn')),h('h2',{id:'sh-h',text:o.title})),o.text&&h('p',{class:'muted',id:'sh-p',text:o.text}),f);o.text?d.setAttribute('aria-describedby','sh-p'):d.removeAttribute('aria-describedby');d.showModal();(o.focus||no).focus()}
const fld=(l,i)=>h('div',{class:'f'},h('label',{for:i.id,text:l}),i);
function shell(){const m=S.m,st=off?['crit','Not answering']:!m?['off','Connecting']:m.ready?['ok','Online']:['warn','Starting'],a=S.c?.bind||location.hostname;$$('.cam-name').forEach(e=>e.textContent=name());$$('.cam-addr').forEach(e=>e.textContent=a);$$('.state').forEach(e=>e.replaceChildren(pill(...st)));tx('sy-cmd',`ssh ${user}@${a}`);title()}
function rMedia(m){shell();ov();pos();presets();lights();audio();tx('sy-mv',m.version+(rel()?' · release '+rel():'')+' installed')}
function rConfig(){shell();streams();orient();audio();ov()}
function rJoan(j){shell();ov();tx('sy-pv',j.version+' installed');tx('sy-pws','This page and SSH. '+(j.default_password_warning?'Initial value.':'Changed.'));$('sy-sync').hidden=j.ssh_password_sync!==false}
function rName(n){shell();streams();document.activeElement!=$('nw-host')&&($('nw-host').value=n.hostname)}
function spec(ch){const s=S.m?.streams?.find(x=>x.channel==ch),c=S.c;return s?`${s.width} × ${s.height}`+(c?` · ${c.fps} fps · H.264 · ${ch?c.bitrate1:c.bitrate} kbit/s`:''):''}
function ntp(n){return!n?.server?['Off']:n.synchronized?['Synced','Synced '+dur(n.last_sync_age_seconds+age('j')/1e3)+' ago']:['Not synced']}
function hl(){const m=S.m,j=S.j,w=S.q?.wifi,L=[],a=(t,k,v)=>L.push([t,k,v]);
if(m){const p=m.platform||{},A=m.audio||{},mc=A.microphone||{},sp=A.speaker||{};a(m.ready?'ok':m.backend_error?'crit':'warn','Media service',m.ready?'Running · '+dur(m.uptime_seconds):m.backend_error||'Starting');m.restart_pending&&a('warn','Video','Restarting');a(!p.homing&&!p.last_error&&p.position_known?'ok':'warn','Pan/tilt',p.homing?'Calibrating':p.last_error||(p.position_known?'Ready':'Position unknown'));
if(!A.enabled)a('off','Audio','Off');else{a(mc.error||mc.state!='running'?'warn':'ok','Microphone',mc.error||cap(mc.state||'unknown'));a(sp.error||sp.faults?'warn':'ok','Speaker',sp.error||(sp.faults?pl(sp.faults,'fault'):cap(sp.state||'ready')))}}
w&&a(w.associated&&w.signal_dbm>=-70?'ok':'warn','Wi-Fi',w.associated?(sig(w.signal_dbm)||'Connected')+(w.signal_dbm!=null?' · '+mi(w.signal_dbm)+' dBm':''):'Not connected');
if(j){const n=j.ntp||{},[c,d]=ntp(n);a(!n.server?'off':n.synchronized?'ok':'warn','Clock',n.synchronized?d:c);a(j.ssh_password_sync===false?'warn':'ok','SSH password',j.ssh_password_sync===false?'Not synchronized':'In sync');a(j.state=='ready'?'ok':'warn','Platform',j.version+(j.state=='ready'?'':' · '+j.state))}return L}
const hlist=()=>hl().map(([t,k,v])=>h('li',null,h('span',{class:'ico '+t},ico(t)),h('span',{text:k}),h('span',{text:v})));
function ov(){tx('ov-s0',spec(0));tx('ov-s1',spec(2));const L=hl(),n=L.filter(x=>x[0]=='warn'||x[0]=='crit').length;$('ov-stats').replaceChildren(...hlist());$('ov-sum').replaceChildren(off?pill('crit','Not answering'):L.length?n?pill('warn',pl(n,'thing')+' to check'):pill('ok','All normal'):'')}
function lights(){const p=S.m?.platform;if(!p)return;$$('.night').forEach(s=>busy(s)||pick(s,p.night));$$('.sw.wl').forEach(b=>busy(b)||sw(b,p.white_light==1));$$('.wl-note').forEach(e=>e.textContent=p.white_light<0?'Not set since the camera started':p.white_light?'On':'Off')}
$$('.night').forEach(s=>seg(s,v=>run(s,async()=>{await ctl('night',+v);S.m&&(S.m.platform.night=+v);lights();toast(+v?'Night vision on':'Day mode on')})));
$$('.sw.wl').forEach(b=>b.onclick=()=>{const on=b.getAttribute('aria-checked')!='true';run(b,async()=>{await ctl('white_light',+on);S.m&&(S.m.platform.white_light=+on);lights();toast('White light '+(on?'on':'off'))})});
const PIC=['brightness','contrast','saturation','sharpness'],pv=[{},{}];
PIC.forEach(op=>$('im-sl').append(slider('im-'+op,cap(op),0,255,128,String,async v=>{const l=lens;await ctl(op,v,l);pv[l][op]=v})));
seg($('im-lens'),v=>{lens=+v;pick($('im-lens'),v);PIC.forEach(op=>setSl('im-'+op,pv[lens][op]??128,1))});
$('im-reset').onclick=act(async()=>{for(const op of PIC){await ctl(op,128,lens);pv[lens][op]=128;setSl('im-'+op,128,1)}toast('Picture reset to neutral')});
const rm=f=>[f>>1&1,(f^f>>1)&1];
function orient(){const c=S.c;if(!c)return;$('im-or').classList.toggle('one',c.sensors<2);$$('#im-or .sw').forEach(b=>{if(busy(b))return;const[R,M]=rm(c['flip'+b.dataset.s]|0);sw(b,b.dataset.k=='r'?R:M)})}
$('im-or').onclick=e=>{const b=e.target.closest('.sw');if(!b||!S.c)return;const s=+b.dataset.s;let[R,M]=rm(S.c['flip'+s]|0);b.dataset.k=='r'?R^=1:M^=1;const f=(R?3:0)^M;run(b,async()=>{await ctl('flip',f,s);S.c['flip'+s]=f;orient();toast('Saved. Video restarts for a few seconds.');setTimeout(()=>config().catch(noop),5e3)})};
/* ptz: a velocity with a 1.2 s lease, renewed while held; ptz_stop on release, in order. */
function send(op,x,y){pn++;pq=pq.then(()=>ctl(op,x,y)).catch(e=>{if(op!='ptz')return;ptzEnd(1);e.kind!='auth'&&err($('pt-stop'),e.kind=='off'?OFF:e.message)}).finally(()=>pn--)}
function ptzGo(b){if(pb==b||!csrf||off)return;pb&&pb.classList.remove('on');clr($('pt-stop'));pb=b;b.classList.add('on');const[x,y]=b.dataset.d.split(',').map(n=>n*60);send('ptz',x,y);clearInterval(pt);pt=setInterval(()=>pn||send('ptz',x,y),400);fast=Date.now()+4e3}
function ptzEnd(q){if(!pb)return;clearInterval(pt);pb.classList.remove('on');pb=null;fast=Date.now()+3e3;q!==1&&send('ptz_stop')}
function hold(b,dn,up){b.addEventListener('pointerdown',e=>{if(e.button)return;e.preventDefault();b.setPointerCapture(e.pointerId);dn()});['pointerup','pointercancel','lostpointercapture'].forEach(t=>b.addEventListener(t,up));
const k=e=>e.key==' '||e.key=='Enter';b.onkeydown=e=>{if(k(e)){e.preventDefault();e.repeat||dn()}};b.onkeyup=e=>{if(k(e)){e.preventDefault();up()}};b.oncontextmenu=e=>e.preventDefault()}
$$('#pt-pad [data-d]').forEach(b=>hold(b,()=>ptzGo(b),ptzEnd));
$('pt-stop').onclick=()=>{ptzEnd(1);send('ptz_stop')};
const AR={ArrowUp:'u',ArrowDown:'d',ArrowLeft:'l',ArrowRight:'r'};
$('pt-move').onkeydown=e=>{const s=AR[e.key];if(!s||e.target.closest('.seg'))return;e.preventDefault();e.repeat||ptzGo($('pt-pad').querySelector('.'+s))};
$('pt-move').onkeyup=e=>AR[e.key]&&!e.target.closest('.seg')&&ptzEnd();
addEventListener('blur',ptzEnd);
function pos(){const p=S.m.platform,t=p.position_known?`Pan ${sg(p.position_x)} · Tilt ${sg(p.position_y)}`:'Position unknown',l=t+' · '+(p.homing?'calibrating':p.moving?'moving':'stopped');$$('.pos').forEach(e=>e.textContent=t);$('pt-pos').textContent!=l&&tx('pt-pos',l)}
const P=()=>S.m?.platform?.presets||[];
function presets(){const L=P(),k=JSON.stringify(L);if(k==lastP)return;lastP=k;
$('pt-list').replaceChildren(...L.map(p=>h('li',null,h('div',null,h('i',{text:p2(p.slot)}),h('b',{text:p.name})),h('div',null,h('button',{class:'btn','aria-label':'Go to '+p.name,onclick:act(async()=>{await ctl('preset_goto',p.slot);fast=Date.now()+2e4;toast(`Moving to “${p.name}”`);media().catch(noop)})},'Go'),h('button',{class:'btn','aria-label':'More actions for '+p.name,'aria-haspopup':'dialog',onclick:()=>menu(p)},ico('more'))))));
$('pt-list').hidden=!L.length;$('pt-empty').hidden=!!L.length;tx('pt-count',L.length?`${L.length} of 16 used. Saving takes the first free slot.`:'')}
const chk=v=>(v=v.trim())||no('Enter a name.');
function nameSheet(slot,title,text,ok,value,extra){const i=h('input',{id:'sh-n',value,maxlength:64,autocomplete:'off'});sheet({title,text,icon:'pantilt',body:[fld('Name',i)],ok,focus:i,extra,run:async()=>{const n=chk(i.value,slot);await ctl('preset_set',slot,0,n);toast(`Saved “${n}” in slot ${slot}`);lastP='';await media()}})}
function save(){const u=P().map(p=>p.slot),s=[...Array(16).keys()].map(i=>i+1).find(s=>!u.includes(s));if(!s)return err($('pt-save'),'All 16 slots are in use. Delete one first.');nameSheet(s,'Save this position',`Saves where the pan/tilt lens points now, in slot ${s}.`,'Save preset','Preset '+s)}
function menu(p){nameSheet(p.slot,p.name,`Slot ${p.slot}. Saving stores where the lens points now: to rename, go to the preset first.`,'Save current position here',p.name,h('button',{type:'button',class:'btn dz',onclick:()=>sheet({title:`Delete “${p.name}”?`,text:`Slot ${p.slot} becomes free. Your NVR loses this preset too.`,ok:'Delete',danger:1,icon:'pantilt',run:async()=>{await ctl('preset_remove',p.slot);toast('Preset deleted');lastP='';await media()}})},'Delete preset…'))}
$('pt-save').onclick=save;
const vf=v=>v+' · '+(v==60?'unity':(v>60?'+':'−')+Math.abs(v-60)/2+' dB'),aon=()=>S.c?S.c.audio:S.m?.audio?.enabled;
$('au-msl').append(slider('au-mv','Volume',-30,120,80,vf,v=>ctl('volume',v,0)),slider('au-mg','Analog gain',0,31,22,String,v=>ctl('gain',v,0)));
$('au-ssl').append(slider('au-sv','Volume',-30,120,60,vf,v=>ctl('volume',v,1)),slider('au-sg','Analog gain (from the next talk)',0,31,25,String,v=>ctl('gain',v,1)));
function audio(){const A=S.m?.audio||{},mc=A.microphone||{},sp=A.speaker||{},en=aon(),k=!!(S.m||S.c);$('au-body').hidden=k&&!en;$('au-off').hidden=!k||!!en;setSl('au-mv',mc.volume);setSl('au-mg',mc.gain);setSl('au-sv',sp.volume);setSl('au-sg',mc.speaker_gain);
$('au-mst').replaceChildren(mc.state?pill(mc.error||mc.state!='running'?'warn':'ok',mc.error?'Error':cap(mc.state)):'');$('au-sst').replaceChildren(sp.state?pill(sp.error||sp.faults?'warn':'ok',sp.error?'Error':cap(sp.state)):'');talkUI()}
/* G.711 A-law (ITU reference coder): 0→0xD5, -1→0x55, 32767→0xAA, -32768→0x2A */
function alaw(s){let m=213,g=0;s>>=3;if(s<0){m=85;s=-s-1}while(g<8&&s>(32<<g)-1)g++;return(g>7?127:g<<4|(g<2?s>>1:s>>g)&15)^m}
const T={st:'idle',q:[],n:0,tk:0},TB=[];
$$('.talk-box').forEach(x=>{const b=h('button',{class:'talk','aria-pressed':'false'},ico('mic'),h('span',null,h('span'),h('small')),h('span',{class:'mono'})),st=h('span',{role:'status'});x.append(b,st);TB.push([b,st]);hold(b,talkGo,talkStop)});
function talkUI(e){if(e!=null)T.err=e;const on=T.st=='on'||T.st=='stopping',w=T.st=='starting',v=S.m?.audio?.speaker?.volume,en=aon();
for(const[b,st]of TB){const[a,c]=b.children[1].children;b.setAttribute('aria-pressed',on);b.disabled=en===false&&T.st=='idle';a.textContent=w?'Starting…':on?'Talking':'Hold to talk';c.textContent=on?'Release to stop':'';on||(b.children[2].textContent='');
st.textContent=T.err||(on?`Your voice plays through ${name()}’s speaker${v!=null?' at volume '+v:''}.`:w?'Waiting for the microphone…':en===false?'Audio is off on the camera.':'Ready. Hold the button, or Space while it has focus.')}}
async function talkGo(){if(T.st!='idle'||!csrf||off)return;const AC=window.AudioContext||window.webkitAudioContext,tk=++T.tk;if(!isSecureContext||!navigator.mediaDevices?.getUserMedia||!AC)return talkUI('Talk needs the HTTPS page and a browser microphone.');
Object.assign(T,{st:'starting',q:[],n:0,ok:0,n409:0,err:''});talkUI();
try{const ctx=T.ctx=new AC(),s=await navigator.mediaDevices.getUserMedia({audio:{channelCount:1,echoCancellation:true,noiseSuppression:true,autoGainControl:true}});if(tk!=T.tk||T.st!='starting'){s.getTracks().forEach(t=>t.stop());ctx.close();return}T.s=s;await ctx.resume();
const p=T.p=ctx.createScriptProcessor(4096,1,1),g=ctx.createGain(),r=ctx.sampleRate/8e3;let acc=0,cnt=0,ph=0;g.gain.value=0;ctx.createMediaStreamSource(s).connect(p);p.connect(g);g.connect(ctx.destination);
p.onaudioprocess=ev=>{if(T.st!='on')return;const x=ev.inputBuffer.getChannelData(0),o=new Uint8Array(x.length/r+1|0);let k=0;for(const v0 of x){acc+=v0;cnt++;if(++ph>=r){ph-=r;const v=Math.max(-1,Math.min(1,acc/cnt));acc=cnt=0;o[k++]=alaw(Math.round(v*32767))}}T.q.push(o.subarray(0,k));T.n+=k;while(T.n>8e3)T.n-=T.q.shift().length;pump()};
T.st='on';T.t0=Date.now();clearInterval(T.iv);T.iv=setInterval(meter,500);talkUI()}catch(e){tk==T.tk&&end(e?.name=='NotAllowedError'?'The browser blocked the microphone. Allow it for this page.':e?.name=='NotFoundError'?'This device has no microphone.':'The microphone could not start.')}}
function meter(){const s=(Date.now()-T.t0)/1e3|0;for(const[b]of TB)b.children[2].textContent=(s/60|0)+':'+p2(s%60)}
async function pump(){if(T.sending)return;T.sending=1;try{while(T.n&&(T.n>=2e3||T.st=='stopping')){const n=Math.min(T.n,4e3),o=new Uint8Array(n);for(let k=0;k<n;){const c=T.q[0],t=Math.min(c.length,n-k);o.set(c.subarray(0,t),k);k+=t;t==c.length?T.q.shift():T.q[0]=c.subarray(t)}T.n-=n;
try{await api(M+'talk',{method:'POST',body:o,type:'audio/PCMA',timeout:4e3});T.ok=1;T.n409=0}catch(e){if(e.st==409&&T.ok&&!/another/.test(e.raw)&&++T.n409<4)continue;end(e.kind=='off'?'The camera stopped answering.':e.message);break}}}finally{T.sending=0;if(T.st=='stopping'&&!T.n){T.st='idle';talkUI()}}}
function tclose(){clearInterval(T.iv);try{T.s?.getTracks().forEach(t=>t.stop());T.p&&(T.p.onaudioprocess=null);T.ctx?.close()}catch(_){}T.s=T.p=T.ctx=null}
function talkStop(){if(T.st=='starting'){T.st='idle';tclose();talkUI()}if(T.st!='on')return;tclose();T.st='stopping';talkUI();pump()}
function end(m){tclose();Object.assign(T,{st:'idle',q:[],n:0});talkUI(m||'')}
$('au-tone').onclick=act(async()=>{if(T.st!='idle')no('Stop talking first.');const b=new Uint8Array(8e3);for(let i=0;i<8e3;i++)b[i]=alaw(Math.round(Math.sin(i*.34558)*9e3*Math.min(1,i/240,(8e3-i)/240)));await api(M+'talk',{method:'POST',body:b,type:'audio/PCMA',timeout:5e3});toast('Playing a 1-second test tone');await new Promise(r=>setTimeout(r,1e3))});
document.addEventListener('visibilitychange',()=>{if(document.hidden){talkStop();ptzEnd()}else csrf&&!off&&load(route)});
addEventListener('pagehide',()=>{talkStop();ptzEnd()});
function streams(){const c=S.c;if(!c)return;const H=[...new Set([c.bind,location.hostname,S.n?.address].filter(x=>x&&x!='0.0.0.0'))],el=$('st-host'),rp=c.rtsp_port||18554;if(!H.includes(hs))hs=H[0];if(el.dataset.k!=H){el.dataset.k=H;el.replaceChildren(...H.map(x=>h('button',{value:x,class:'mono'},x)))}pick(el,hs);
const ln=(nm,ch)=>h('div',{class:'stack s'},h('div',null,h('h3',{text:nm}),h('small',{class:'mono',text:spec(ch)})),h('div',{class:'f'},h('span',{class:'lbl',text:'RTSP'}),cf(`rtsp://${hs}:${rp}/live/ch${ch}`,nm.toLowerCase()+' RTSP address')));
$('st-lenses').replaceChildren(ln('Pan/tilt lens',0),c.sensors>1&&ln('Fixed lens',2));tx('st-vs',`RTSP on port ${rp} with Digest sign-in, ONVIF on 18443 over HTTPS. No JPEG stills in this video profile.`);$('st-onvif').replaceChildren(cf(`https://${hs}:18443/onvif/device_service`,'ONVIF device address'));
tx('st-on','Pan/tilt and 16 presets over ONVIF. '+(c.discovery?'Discovery is on.':'Discovery is off: add the camera by address.')+(c.main_only?' ch1 and ch3 repeat ch0 and ch2.':''));$('st-user').replaceChildren(cf(c.username,'stream user name'));tx('st-pw',c.password_set?'Set on the camera; never shown here.':'Not set.');tx('st-yaml',yaml(rp,c))}
seg($('st-host'),v=>{hs=v;streams()});$('st-copy').onclick=e=>copy($('st-yaml').textContent,e.currentTarget,$('st-yaml'));
function yaml(rp,c){const a=c.audio,u=c.username,fx=c.sensors>1,src=(ch,x)=>`rtsp://${u}:<password>@${hs}:${rp}/live/ch${ch}${a?x||'#backchannel=0':''}`,
cam=(n,m)=>[`  ${n}:`,'    ffmpeg:',...a?['      output_args:','        record: preset-record-generic-audio-aac']:[],'      inputs:',`        - path: rtsp://127.0.0.1:8554/${n}`,'          input_args: preset-rtsp-restream','          roles: [record, detect]',...m?[...a?['    live:','      streams:','        Main: jooan_main_talk']:[],'    onvif:',`      host: https://${hs}`,'      port: 18443',`      user: ${u}`,'      password: "<password>"','      profile: main0']:[]];
return['go2rtc:','  streams:','    jooan_main:','      - '+src(0),...a?['    jooan_main_talk:','      - rtsp://127.0.0.1:8554/jooan_main#backchannel=0','      - '+src(0,'#backchannel=1#media=audio')]:[],...fx?['    jooan_fixed:','      - '+src(2)]:[],'cameras:',...cam('jooan_main',1),...fx?cam('jooan_fixed'):[]].join('\n')+'\n'}
function rQuality(q){const w=q.wifi||{},e=q.ethernet||{},d=w.signal_dbm,r=w.rate_mbps;
$('nw-now').replaceChildren(h('span',{class:'ico '+(w.associated?'ok':'warn')},ico(w.associated?'ok':'warn')),h('span',null,h('b',{text:w.associated?'Connected to '+(w.ssid||'Wi-Fi'):'Not connected to Wi-Fi'}),h('small',{text:[d!=null&&mi(d)+' dBm, '+sig(d).toLowerCase(),r!=null&&+r+' Mb/s',S.c&&'address '+S.c.bind,'sent '+bytes(w.tx_bytes)+', received '+bytes(w.rx_bytes)+' since the restart',e.link?'Ethernet '+(e.speed_mbps||'')+' Mb/s':'no Ethernet cable'].filter(Boolean).join(' · ')})));

joining||tx('nw-help',`The camera leaves ${w.ssid||'its network'} for the new one. Confirm within 90 s from a device that reaches it there, or it goes back and restarts.`);ov()}
$('nw-join').onsubmit=e=>{e.preventDefault();const s=$('nw-ssid'),p=$('nw-psk'),id=s.value,k=p.value,bad=(el,m)=>{el.setAttribute('aria-invalid','true');err(el,m);el.focus()};
if(!id.trim()||new TextEncoder().encode(id).length>32)return bad(s,'Enter the network name, up to 32 characters.');if(!(k.length>7&&k.length<64||/^[0-9a-f]{64}$/i.test(k)))return bad(p,'Wi-Fi passwords have 8 to 63 characters.');
run(sb(e),async()=>{const t0=Date.now(),prev=S.q?.wifi?.ssid||'';let d=null;joining=1;tx('nw-help',`Joining ${id}. This can take up to a minute; keep this page open.`);
try{d=await api(J+'network/wifi',{method:'POST',json:{ssid:id,password:k},timeout:75e3,quiet:1})}catch(x){if(x.kind!='off')throw x}finally{joining=0}
const sec=d&&(d.deadline_seconds??d.seconds_left);W={id:d?.id||'',ssid:id,prev,deadline:sec>0?Date.now()+sec*1e3:t0+9e4,lost:!d};ls.set(WIFI,W);p.value='';wifi();$('wc-h').focus();d||goOff()},p)};
function wifi(){$('p-network').classList.toggle('wc',!!W);$('nw-join').hidden=!!W;$('nw-count').hidden=!W;if(!W)return;const L=Math.max(0,W.deadline-Date.now()),s=Math.ceil(L/1e3),pv=W.prev||'the previous network',n=name(),E=W.end;
if(!E&&!L)return wend('Time is up',`${n} goes back to ${pv} and restarts (about 90 s).`);
tx('wc-h',E?W.eT:`Keep ${W.ssid}?`);tx('wc-n',(s/60|0)+':'+p2(s%60));$('wc-t').setAttribute('aria-label',`${s/60|0} min ${s%60} s left to decide`);$('wc-t').hidden=!!E;$('wc-arc').style.strokeDashoffset=452.4*(1-L/9e4);
tx('wc-p',E?W.eP:W.lost?`The camera left this device’s network. To keep ${W.ssid}, open this page from a device on it in time; otherwise it goes back to ${pv} and restarts.`:`${n} is trying ${W.ssid}. If nobody confirms, it goes back to ${pv} and restarts.`);
tx('wc-keep',E?'Done':'Keep '+W.ssid);tx('wc-back',`Go back to ${pv} now`);$('wc-keep').hidden=!E&&!W.id;$('wc-back').hidden=!!E||!W.id}
function wend(t,p){Object.assign(W,{end:1,eT:t,eP:p});ls.set(WIFI);wifi()}
$('wc-keep').onclick=e=>{if(W.end){W=null;wifi();place();return $('nw-hw').focus()}run(e.currentTarget,async()=>{await api(J+'network/wifi',{method:'PUT',json:{id:W.id},timeout:2e4});wend(`${name()} stays on ${W.ssid}`,'If the camera’s address changed, update your NVR.');quality().catch(noop)})};
$('wc-back').onclick=()=>{const pv=W.prev||'the previous network';sheet({title:`Go back to ${pv} now?`,text:`The camera drops ${W.ssid} and restarts (about 90 s).`,ok:'Go back',danger:1,icon:'network',run:async()=>{await api(J+'network/wifi',{method:'DELETE',json:{id:W.id},timeout:2e4});wend('Going back to '+pv,'The camera is restarting; this page reconnects by itself.')}})};
$('nw-name').onsubmit=e=>{e.preventDefault();const i=$('nw-host'),v=i.value.trim();if(!v)return bad(i,CODES.invalid_hostname);run(sb(e),async()=>{const d=await api(J+'network/mdns',{method:'PUT',json:{hostname:v}}),u=`https://${d.address}${location.port?':'+location.port:''}/`;$('nw-res').replaceChildren('Saved. ',h('a',{href:u,class:'mono',text:u}),' works after the next restart.')},i)};
const putR=L=>api(J+'network/routes',{method:'PUT',json:{cidrs:L.join(', ')},timeout:2e4}).then(routes);
function rRoutes(d){const L=d?.cidrs||[];$('nw-routes').replaceChildren(...L.length?L.map(c=>h('li',null,h('code',{text:c}),h('button',{class:'btn','aria-label':'Remove '+c,onclick:e=>L.length<2?err(e.currentTarget,'The list cannot be empty: add the replacement first.'):sheet({title:`Remove ${c}?`,text:`Devices on ${c} lose access to the camera, this one too if it is on ${c}.`,ok:'Remove',danger:1,icon:'network',run:async()=>{await putR(L.filter(x=>x!=c));toast('Removed '+c)}})},'Remove'))):[h('li',{class:'none',text:'None: only the camera’s own network.'})])}
$('nw-add').onsubmit=e=>{e.preventDefault();const i=$('nw-cidr'),v=i.value.trim();if(!v)return bad(i,CODES.invalid_routes);run(sb(e),async()=>{await putR([...(S.r?.cidrs||[]).filter(x=>x!=v),v]);i.value='';toast('Added '+v)},i)};
const TZ=[...Array(27).keys()].map(i=>i*60-720).concat(-570,-210,210,270,330,345,390,525,570,630,765).sort((a,b)=>a-b),dz=-new Date().getTimezoneOffset(),hh=m=>p2(Math.abs(m)/60|0)+':'+p2(Math.abs(m)%60);
TZ.includes(dz)||TZ.push(dz)&&TZ.sort((a,b)=>a-b);
$('sy-tz').append(...TZ.map(m=>h('option',{value:'GMT'+(m<0?'-':'+')+hh(m),selected:m==dz},'UTC'+(m<0?'−':'+')+hh(m)+(m==dz?' (this device)':''))));$('sy-tz').onchange=()=>tzd=1;
function seed(){$('sy-man').value=new Date(Date.now()-new Date().getTimezoneOffset()*6e4).toISOString().slice(0,19)}
const step=ms=>{const a=Math.abs(ms);return a<1e3?Math.round(a)+' ms':a<6e4?(a/1e3).toFixed(1)+' s':dur(a/1e3)};
function rTime(d){const n=d.ntp||{};if(!filled){$('sy-ntp').value=n.server||'';filled=1}tx('sy-nh','“gateway”, an IPv4 address, or empty for off. '+(!n.server?'Network time is off now.':n.synchronized?`Synced ${dur(n.last_sync_age_seconds)} ago with ${n.resolved||n.server}${n.last_offset_ms!=null?'; last correction '+step(n.last_offset_ms):''}.`:`Not synced yet: ${n.error||'waiting for the first reply'}.`));tick()}
function tick(){if(route!='system')return;const now=new Date;tx('sy-dev',hm(now));if(!S.t)return;const c=new Date(S.t.epoch*1e3+age('t')),df=Math.round(Math.abs(c-now)/1e3);tx('sy-cam',c.toLocaleString([],{dateStyle:'medium',timeStyle:'medium'}));$('sy-match').replaceChildren(df<3?pill('ok','matches'):pill('warn','differs by '+dur(df)))}
const putT=json=>api(J+'time',{method:'PUT',json,timeout:15e3}),putZ=()=>api(J+'timezone',{method:'PUT',json:{gmt_tz:$('sy-tz').value},timeout:15e3}).then(()=>tzd=0);
$('sy-save').onclick=act(async()=>{const n=$('sy-ntp').value.trim();let did=0;if(n!=(S.t?.ntp?.server||'')){await putT({ntp:n});did=1}if(tzd){await putZ();did=1}toast(did?'Saved':'Nothing to save');filled=0;await Promise.all([clock(),joan()])});
$('sy-now').onclick=act(async()=>{await putT({epoch:String(Date.now()/1e3|0)});await putZ();toast('Clock and time zone set from this device');await clock()});
$('sy-set').onclick=act(async()=>{const v=$('sy-man').value,t=new Date(v).getTime();if(!v||!isFinite(t))no('Choose a date and time first.');await putT({epoch:String(t/1e3|0)});toast('Camera clock set');await clock()},$('sy-man'));
function pwSheet(){const o=pwf('Current password','current-password'),n=pwf('New password','new-password'),r=pwf('Repeat the new password','new-password');
sheet({title:'Change the admin password',text:'Used for this page and for SSH. You sign in again afterwards.',icon:'lock',ok:'Change password',focus:o.i,body:[h('input',{autocomplete:'username',value:user,hidden:true}),o.f,n.f,r.f],
run:async()=>{if(!o.i.value)no('Enter the current password.');if(!n.i.value)no('Enter a new password.');if(n.i.value!=r.i.value)no('The new passwords do not match.');
await api(J+'setup/password',{method:'POST',json:{old_password:o.i.value,new_password:n.i.value},timeout:2e4});$('sheet').close();out('Password changed. Sign in with the new password.')}})}
$('sy-pwa').onclick=$('sy-syncb').onclick=pwSheet;
function rKeys(d){const it=String(d?.authorized_keys||'').split('\n').filter(l=>l.trim()).map(l=>{const[t,b,...c]=l.trim().split(/\s+/),nm=c.join(' ')||t;return h('li',null,h('span',null,h('b',{text:nm}),h('small',{class:'mono',text:t.slice(4)+' · …'+(b||'').slice(-12)})),h('button',{class:'btn dz','aria-label':'Remove the key '+nm,onclick:rmKey},'Remove…'))});$('sy-keys').replaceChildren(...it.length?it:[h('li',{class:'none',text:'No key. SSH uses the admin password.'})])}
const rmKey=()=>sheet({title:'Remove the SSH key?',text:'SSH then accepts only the admin password.',ok:'Remove',danger:1,icon:'lock',run:async()=>{await api(J+'ssh/authorized-keys',{method:'DELETE',json:{id:'all'},timeout:2e4});toast('SSH key removed');await keys()}});
$('sy-add').onsubmit=e=>{e.preventDefault();const t=$('sy-key'),v=t.value.trim().replace(/\s+/g,' ');if(!v.startsWith('ssh-ed25519 ')){t.setAttribute('aria-invalid','true');return err(t,'Paste one key starting with ssh-ed25519.')}
const add=async()=>{await api(J+'ssh/authorized-keys',{method:'POST',body:v,type:'text/plain',timeout:2e4});t.value='';toast('SSH key added');await keys()};S.k?.authorized_keys?.trim()?sheet({title:'Replace the SSH key?',text:'The camera keeps one key, so the current key stops working.',ok:'Replace key',danger:1,icon:'lock',run:add}):run(sb(e),add,t)};
function fwUI(st,m){$('sy-prog').hidden=st!=1;tx('sy-ust',m||'');$('sy-apply').disabled=!fw;tx('sy-apply',fw?'Install and restart…':'Install (upload first)')}
function upload(f){return take().then(()=>new Promise((ok,no)=>{const x=new XMLHttpRequest();x.onloadend=give;x.open('POST',J+'update');[['X-Requested-With','jipc-ui'],['X-CSRF-Token',csrf],['Content-Type','application/octet-stream'],['Accept','application/json']].forEach(([k,v])=>x.setRequestHeader(k,v));x.timeout=18e4;
x.upload.onprogress=e=>{if(e.lengthComputable){$('sy-pb').value=e.loaded/e.total*100;tx('sy-pn',bytes(e.loaded)+' of '+bytes(e.total))}};x.upload.onload=()=>fwUI(2,'Checking the signature (up to a minute). Keep this page open.');
x.onload=()=>{let d=null;try{d=JSON.parse(x.responseText)}catch(_){}if(x.status>199&&x.status<300)return ok(d);if(x.status==401){out('Your session ended. Sign in again.');return no(new Fail('','auth'))}const er=d?.error;no(new Fail(x.status==502?'The camera refused this package: it must be a signed, newer platform release for this camera.':say(x.status,er?.code,typeof er=='string'?er:er?.message||'')))};
x.onerror=x.ontimeout=()=>no(new Fail('The upload stopped: the camera is not answering.'));x.send(f)}))}
$('sy-up').onsubmit=e=>{e.preventDefault();const f=$('sy-file').files[0];if(!f)return err($('sy-file'),'Choose the package file first.');if(f.size>2097344)return err($('sy-file'),'Too large for a platform package (2 MB).');fw='';tx('sy-pt','Uploading '+f.name);fwUI(1);
run($('sy-stage'),async()=>{try{const d=await upload(f);if(!d?.id)no('The camera did not stage the package. Try again.');fw=d.id;fwUI(3,`Signature verified${d.version?': platform '+d.version:''}. Ready to install.`)}catch(x){fwUI(0);throw x}},$('sy-file'))};
$('sy-apply').onclick=()=>fw&&sheet({title:'Install and restart?',text:'The camera installs the verified package and restarts; it is offline for about 90 seconds.',ok:'Install and restart',danger:1,icon:'system',run:async()=>{try{await api(J+'update',{method:'PUT',json:{id:fw},timeout:6e4,quiet:1})}catch(x){if(x.kind!='off')throw x}fw='';$('sy-file').value='';fwUI(4,'Installing. The camera restarts; this page reconnects by itself.');toast('Installing. The camera restarts now.')}});
function theme(t){const r=document.documentElement;t=='light'||t=='dark'?r.dataset.theme=t:(delete r.dataset.theme,t='auto');ls.set(THEME,t=='auto'?null:t);$$('.theme').forEach(s=>pick(s,t))}
$$('#side a,#tabs a,.menu a').forEach(a=>{const i=ico(a.hash.slice(1));a.closest('#tabs')?a.prepend(h('span',null,i)):a.prepend(i);a.closest('.menu')&&a.append(ico('chev'))});
$$('svg').forEach(s=>{s.setAttribute('aria-hidden','true');s.hasAttribute('viewBox')||s.setAttribute('viewBox','0 0 24 24')});
$$('.seg').forEach(s=>pick(s));$$('.sw').forEach(b=>{b.setAttribute('role','switch');sw(b,0);b.append(h('span'))});
$$('.reveal').forEach(b=>{b.setAttribute('aria-label','Show password');b.setAttribute('aria-pressed','false')});
$$('.theme').forEach(s=>seg(s,theme));theme(ls.get(THEME));pick($('im-lens'),0);
$$('.raw,[type=password]').forEach(i=>{i.autocomplete=i.autocomplete||'off';i.autocapitalize='off';i.spellcheck=false});$$('.host').forEach(e=>e.textContent=location.host);$$('.cam-name').forEach(e=>e.textContent=guess);
setInterval(()=>{tick();W&&!W.end&&wifi();if(!csrf||document.hidden)return;if(off){tx('bn-s',Math.max(0,Math.ceil((retry-Date.now())/1e3))+' s');Date.now()>=retry&&probe();return}
const mv=fast>Date.now()||S.m?.platform?.moving;if(!['streams','network','settings'].includes(route)&&age('m')>(mv&&route=='pantilt'?1e3:5e3))media().catch(noop);
if((route=='overview'||route=='system')&&age('j')>6e4)joan().catch(noop);if(['overview','network','settings'].includes(route)&&age('q')>3e4)quality().catch(noop);
if(age('ka')>24e4){S['ka@']=Date.now();api(J+'session',{quiet:1}).then(d=>d?.csrf&&(csrf=d.csrf)).catch(noop)}},1e3);
(async()=>{try{const d=await api(J+'session',{login:1,quiet:1});if(!d?.csrf)throw 0;csrf=d.csrf;S['ka@']=Date.now();enter()}catch(e){const o=e?.kind=='off';showSignIn(o?NA:'',o&&'crit')}})();
})();
