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
let initCalls=0,fragmentCalls=0,player;
globalThis.window={MediaSource:MediaSourceMock,fetch:async url=>{
  if(url.includes('init.mp4')){initCalls++;return{ok:true,arrayBuffer:async()=>new ArrayBuffer(8),headers:{get:()=>null}};}
  fragmentCalls++;
  if(fragmentCalls<=3)throw new Error('daemon restart');
  queueMicrotask(()=>player.close());
  return{ok:true,arrayBuffer:async()=>new ArrayBuffer(8),headers:{get:()=>"1"}};
}};
globalThis.CustomEvent=class{constructor(type){this.type=type;}};
globalThis.window.dispatchEvent=()=>{};
let settlePlay;
const video={src:'',currentTime:0,removeAttribute(){this.src='';},load(){},play:()=>new Promise(resolve=>{settlePlay=resolve;})};
const {Fmp4Player}=await import('./video-player.js');
player=new Fmp4Player(video,{mime:'video/mp4; codecs="avc1.640032"',init:'/init.mp4',fragment:'/fragment.mp4'});
await player.start();
for(let i=0;i<30&&!fragmentCalls;i++)await new Promise(resolve=>queueMicrotask(resolve));
assert.ok(fragmentCalls>0,'fragment pump must not wait for the play promise to settle');
for(let i=0;i<30&&initCalls<2;i++)await new Promise(resolve=>queueMicrotask(resolve));
assert.ok(initCalls>=2,'player must fetch a new init segment after repeated fragment failures');
assert.equal(player.sequence,0,'reinitialized player resets fragment sequence');
settlePlay?.();
console.log('video-player recovery: PASS');
