import assert from 'node:assert/strict';

class Events {
  constructor(){this.listeners=new Map();}
  addEventListener(name,fn){const list=this.listeners.get(name)||[];list.push(fn);this.listeners.set(name,list);}
  removeEventListener(name,fn){this.listeners.set(name,(this.listeners.get(name)||[]).filter(item=>item!==fn));}
  emit(name){for(const fn of [...(this.listeners.get(name)||[])])fn();}
}
class SourceBuffer extends Events {
  constructor(){super();this.mode='segments';this.updating=false;this.buffered={length:0};}
  appendBuffer(){queueMicrotask(()=>this.emit('updateend'));}
  remove(){queueMicrotask(()=>this.emit('updateend'));}
}
class MediaSourceMock extends Events {
  static isTypeSupported(){return true;}
  constructor(){super();this.readyState='open';queueMicrotask(()=>this.emit('sourceopen'));}
  addSourceBuffer(){return new SourceBuffer();}
  endOfStream(){this.readyState='ended';}
}
globalThis.MediaSource=MediaSourceMock;
globalThis.URL={createObjectURL:()=>`blob:${Math.random()}`,revokeObjectURL:()=>{}};
globalThis.setTimeout=fn=>{queueMicrotask(fn);return 1;};
globalThis.CustomEvent=class{constructor(type){this.type=type;}};
globalThis.window={MediaSource:MediaSourceMock,dispatchEvent:()=>{}};
const {Fmp4Player}=await import('./video-player.js');
const stream={mime:'video/mp4; codecs="avc1.640032"',init:'/init.mp4',fragment:'/fragment.mp4'};
const video=()=>({src:'',currentTime:0,removeAttribute(){this.src='';},load(){},play:()=>new Promise(()=>{})});
const flush=async (predicate)=>{for(let i=0;i<150&&!predicate();i++)await new Promise(resolve=>queueMicrotask(resolve));assert.ok(predicate(),'player did not make expected progress');};
const reply=(sequence)=>({ok:true,arrayBuffer:async()=>new ArrayBuffer(8),headers:{get:()=>sequence?.toString()||null}});

let initCalls=0,fragmentCalls=0,player;
window.fetch=async url=>{
  if(url.includes('init.mp4')){initCalls++;return reply(0);}
  fragmentCalls++;
  if(fragmentCalls<=2)throw new Error('daemon restart');
  queueMicrotask(()=>player.close());
  return reply(1);
};
player=new Fmp4Player(video(),stream);
await player.start();
await flush(()=>fragmentCalls>0);
await flush(()=>initCalls>=2);
assert.equal(player.sequence,0,'reinitialized player resets fragment sequence');
player.close();

// A missed GOP must not be appended to an incompatible MSE timeline.
initCalls=0;fragmentCalls=0;
const seen=[];
window.fetch=async url=>{
  if(url.includes('init.mp4')){initCalls++;return reply(0);}
  seen.push(url);
  fragmentCalls++;
  if(fragmentCalls===1)return reply(25);
  if(fragmentCalls===2)return reply(27); // 26 is missing
  queueMicrotask(()=>player.close());
  return reply(30);
};
player=new Fmp4Player(video(),stream);
await player.start();
await flush(()=>initCalls>=2);
assert.ok(seen[1].includes('after=25'));
assert.equal(player.sequence,0,'a sequence gap must create a new MSE timeline');
player.close();

// A newly opened player seeks to the latest buffered sample immediately.
initCalls=0;fragmentCalls=0;
const liveVideo=video();
window.fetch=async url=>{
  if(url.includes('init.mp4'))return reply(0);
  return reply(35);
};
player=new Fmp4Player(liveVideo,stream);
await player.start();
player.buffer.buffered={length:1,start:()=>0,end:()=>6};
await flush(()=>liveVideo.currentTime===5.3);
player.close();
assert.equal(liveVideo.currentTime,5.3);
console.log('video-player recovery, gaps and live-edge: PASS');
