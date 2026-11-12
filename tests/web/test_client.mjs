import assert from 'node:assert/strict';
import {ApiClient, ApiError, createPoll} from '../../web/app/core.js';
const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
const json = (value, status = 200) => new Response(JSON.stringify(value), {status});
const failure = async (promise, kind) => assert.rejects(promise, error => error instanceof ApiError && error.kind === kind);

// Concurrent operations retain separate activity; mutations serialize Unicode in
// the body and never retry, including when the peer may already have applied it.
let calls = [], active = 0, peak = 0;
const client = new ApiClient({fetcher: async (url, options) => {
  calls.push({url, options}); active++; peak = Math.max(peak, active);
  await sleep(15); active--; return json({ok:true});
}});
const events = [];
client.addEventListener('activity', event => events.push(event.detail));
const first = client.request('/read', {operation:'first'});
const second = client.request('/read', {operation:'second'});
const third = client.request('/save', {method:'POST', body:{name:'A&B +% "雪"'}, operation:'save'});
assert.equal(client.pending.size, 3); await Promise.all([first, second, third]);
assert.equal(peak, 2); assert.equal(client.pending.size, 0);
assert.deepEqual(JSON.parse(calls[2].options.body), {name:'A&B +% "雪"'});
assert.equal(calls[2].url, '/save');
assert.equal(events.filter(event => event.pending === 0).length, 3);
calls = [];
await client.request('/read', {query:{name:'A&B +% "雪"'}});
assert.equal(new URL(calls[0].url,'http://fixture').searchParams.get('name'), 'A&B +% "雪"');
await assert.rejects(client.request('/save', {method:'POST', query:{password:'not-sent'}}), TypeError);
await assert.rejects(client.request('/read', {query:{password:'not-sent'}}), TypeError);
await assert.rejects(client.request('https://elsewhere.invalid/read'), TypeError);

for (const [fetcher, kind] of [
  [async()=>json({},503),'http'], [async()=>new Response('{bad'),'malformed'],
  [async()=>{throw new TypeError('Sensitive transport details must not be exposed');},'network']
]) {
  const api = new ApiClient({fetcher});
  await failure(api.request('/read',{retries:0}),kind);
  assert.equal(api.pending.size,0);assert.equal(api.active,0);
}
const malformed = new ApiClient({fetcher:async()=>json(null)});
await failure(malformed.request('/read',{validate:value=>value.name,retries:0}),'malformed');

const hung = new ApiClient({fetcher:(_url,{signal})=>new Promise((_resolve,reject)=>
  signal.addEventListener('abort',()=>reject(new DOMException('Aborted','AbortError')),{once:true}))});
await failure(hung.request('/read',{timeout:15,retries:0}),'timeout');
const controller = new AbortController();
const pending = hung.request('/read',{signal:controller.signal,retries:0});
await sleep(1);controller.abort();await failure(pending,'abort');
const aborted = new AbortController();aborted.abort();
await failure(hung.request('/read',{signal:aborted.signal}),'abort');
assert.equal(hung.active,0);assert.equal(hung.pending.size,0);

// A queued cancellation must not consume or leak one of the two device slots.
const block = new AbortController();
const occupied = [hung.request('/one',{signal:block.signal,retries:0}),hung.request('/two',{signal:block.signal,retries:0})];
const cancelQueue = new AbortController();
const queued = hung.request('/queued',{signal:cancelQueue.signal,retries:0});
cancelQueue.abort();await failure(queued,'abort');
block.abort();await Promise.all(occupied.map(p=>failure(p,'abort')));
assert.equal(hung.active,0);assert.equal(hung.queue.length,0);

let attempts = 0;
const retry = new ApiClient({fetcher:async()=>{attempts++;if(attempts<2)throw new TypeError('offline');return json({ok:true});}});
await retry.request('/read');assert.equal(attempts,2);
attempts=0;retry.visible=()=>false;
await failure(retry.request('/read'),'network');assert.equal(attempts,1);
let visible=true, hiddenAttempts=0;
const hiddenDuringBackoff=new ApiClient({visibility:()=>visible,fetcher:async()=>{
 hiddenAttempts++;setTimeout(()=>{visible=false;},10);throw new TypeError('offline');
}});
await failure(hiddenDuringBackoff.request('/read'),'network');assert.equal(hiddenAttempts,1);
attempts=0;retry.visible=()=>true;
await assert.rejects(retry.request('/save',{method:'POST',body:{},retries:3}),error=>error.kind==='network'&&error.ambiguous);
assert.equal(attempts,1);

// Visibility changes cancel an active poll. Resume issues one fresh read; it
// cannot replay accumulated intervals or apply an old cancelled response.
class Page extends EventTarget {hidden=false;}
const page = new Page();let reads=0, delivered=0, inFlight=0, maxFlight=0;
const polling = new ApiClient({fetcher:(_url,{signal})=>new Promise((resolve,reject)=>{
  reads++;inFlight++;maxFlight=Math.max(maxFlight,inFlight);
  const timer=setTimeout(()=>{inFlight--;resolve(json({ok:true}));},12);
  signal.addEventListener('abort',()=>{clearTimeout(timer);inFlight--;reject(new DOMException('Abort','AbortError'));},{once:true});
})});
const poll=createPoll(polling,'/read',{document:page,interval:25,onData:()=>delivered++});
poll.start();await sleep(2);page.hidden=true;page.dispatchEvent(new Event('visibilitychange'));
await sleep(70);assert.equal(reads,1);assert.equal(delivered,0);
page.hidden=false;page.dispatchEvent(new Event('visibilitychange'));
await sleep(18);assert.equal(reads,2);assert.equal(delivered,1);assert.equal(maxFlight,1);
poll.stop();await sleep(60);assert.equal(reads,2);

let failures=0;const broken=new ApiClient({fetcher:async()=>json({},503)});
const capped=createPoll(broken,'/read',{document:new Page(),interval:3,maxFailures:3,onData:()=>assert.fail(),onError:()=>failures++});
capped.start();await sleep(70);assert.equal(failures,3);capped.stop();
console.log('PASS request typing, timeout/cancel/queue cleanup, encoding, concurrency, mutation ambiguity, visibility and capped polling');

// Upload transport follows the same timeout/ambiguity/no-retry contract while
// leaving FormData's boundary to the browser. Run with a short injected budget.
{
 const previous=globalThis.XMLHttpRequest;let sends=0,headers;
 globalThis.XMLHttpRequest=class {
  constructor(){this.upload={};headers=this.headers={};}
  open(){}setRequestHeader(k,v){this.headers[k]=v;}
  send(body){sends++;assert.ok(body instanceof FormData);}
  abort(){this.onabort?.();}
 };
 try{
  const client=new ApiClient();const body=new FormData();body.append('file',new Blob(['x']),'test.bin');
  await assert.rejects(client.request('/upload',{method:'POST',body,timeout:20,onUploadProgress:()=>{},retries:3,responseType:'text'}),error=>error.kind==='timeout'&&error.ambiguous===true);
  assert.equal(sends,1);assert.equal(headers['X-Serial2WiFi-Request'],'1');assert.equal(headers['Content-Type'],undefined);assert.equal(client.active,0);
 }finally{globalThis.XMLHttpRequest=previous;}
 console.log('PASS upload XHR: timeout/abort cleanup, ambiguous outcome, one mutation and browser multipart boundary');
}

// Only product preferences are read; invalid or unavailable storage uses defaults.
{
 const {readPreference,savePreference}=await import('../../web/app/core.js');
 const original=Object.getOwnPropertyDescriptor(globalThis,'localStorage'),values=new Map();
 const storage={getItem:k=>values.has(k)?values.get(k):null,setItem:(k,v)=>values.set(k,String(v))};
 Object.defineProperty(globalThis,'localStorage',{configurable:true,value:storage});
 try{
  for(const [name,value,allowed] of [['theme','dark',['dark','light']],['serial-mode','raw',['raw','terminal']],['raw-format','hex',['hex','text']]]){
   savePreference(name,value);assert.equal(readPreference(name,allowed,null),value);
   assert.equal(values.get('esp-serial2wifi-'+name),value);
   savePreference(name,allowed[1]);assert.equal(readPreference(name,allowed,null),allowed[1]);
  }
  values.set('esp-serial2wifi-theme','invalid');assert.equal(readPreference('theme',['dark','light'],'default'),'default');
  values.delete('esp-serial2wifi-theme');storage.setItem=()=>{throw Error('Full');};savePreference('theme','dark');assert.equal(readPreference('theme',['dark','light'],null),null);
  Object.defineProperty(globalThis,'localStorage',{configurable:true,get(){throw Error('Denied');}});assert.equal(readPreference('theme',['dark','light'],'default'),'default');savePreference('theme','dark');
 }finally{if(original)Object.defineProperty(globalThis,'localStorage',original);else delete globalThis.localStorage;}
 console.log('PASS product preferences: persistence, invalid values and storage denial/quota');
}
