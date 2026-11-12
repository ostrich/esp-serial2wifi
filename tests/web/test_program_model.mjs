import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
import {ApiClient,ApiError} from '../../web/app/core.js';
import {prepareHex,ProgramTransaction} from '../../web/app/program-model.js';
const fixtures=JSON.parse(await readFile(new URL('../support/fixtures/api-v1.json',import.meta.url)));
function record(address,type,data){const b=[data.length,address>>8,address&255,type,...data];b.push((-b.reduce((s,n)=>s+n,0))&255);return ':'+b.map(n=>n.toString(16).padStart(2,'0')).join('').toUpperCase();}
const file=record(3,0,[1,2,3])+'\n:00000001FF\n',staged=prepareHex(file);
assert.equal(staged.payloadBytes,3);assert.equal(staged.programBytes,128);assert.equal(staged.pages,1);
const lines=new TextDecoder().decode(staged.hex).trim().split('\n');assert.equal(lines.length,9);
assert(lines[0].startsWith(':10000000FFFFFF010203'));assert.equal(lines.at(-1),':00000001FF');
for(const bad of [file.replace(/FF\n$/,'FE\n'),file+':00000001FF\n',record(32256,0,[1])+'\n:00000001FF',record(0,0,[1])+'\n'+record(0,0,[2])+'\n:00000001FF',record(0,4,[0,1])+'\n'+file,'雪','x'.repeat(262145),record(0,0,[1])])assert.throws(()=>prepareHex(bad));
let raw;
const client=new ApiClient({fetcher:async(path,options)=>{raw={path,...options};return new Response('text reply');}});
assert.equal(await client.request('/pgm/upload',{method:'POST',body:staged.hex,responseType:'text'}),'text reply');
assert(raw.body instanceof Uint8Array);assert.deepEqual(raw.body,staged.hex);assert.equal(raw.headers['Content-Type'],'application/octet-stream');assert.equal(raw.headers['X-Serial2WiFi-Request'],'1');
assert.equal(await client.request('/pgm/sync',{responseType:'text'}),'text reply');
async function run(mode){
 let clock=0,posts=[],calls=[],stages=[];const prepared=prepareHex(file);
 const fake={async request(path,o={}){
  calls.push(path);if(o.method==='POST'){posts.push(path);assert.equal(o.retries,0);}
  if(path==='/api/v1/capabilities'){const c=structuredClone(fixtures[path]);if(mode==='unsupported')c.features.optiboot_readback=false;return c;}
  if(path==='/api/v1/serial'){const c=structuredClone(fixtures[path]);if(mode==='clients')c.clients=1;if(mode==='queued')c.queued_bytes=1;return c;}
  assert.equal(o.responseType,'text');
  if(path==='/pgm/sync'){
   if(o.method==='POST'){assert.equal(o.body.get('verify'),'readback');return '';}
   if(mode==='timeout')return 'NOT READY';if(mode==='sync-fail')return 'FAILED to SYNC';
   return 'SYNC at 115200 baud: bootloader v8.0'+(mode==='ack-only'?'':'; readback enabled');
  }
  assert.equal(path,'/pgm/upload');assert(o.body instanceof Uint8Array);assert.deepEqual(o.body,prepared.hex);
  if(mode==='disconnect')throw new ApiError('network',0,true);
  return 'Success. 128 bytes at 115200 baud; readback verified '+(mode==='bad-result'?'127':'128')+' bytes.';
 }};
 const tx=new ProgramTransaction(fake,{now:()=>clock,pause:async ms=>{clock+=ms;}});tx.addEventListener('stage',e=>stages.push(e.detail.state));
 let error;try{await tx.run(prepared);}catch(e){error=e;}
 if(mode==='normal'){assert(!error);assert.deepEqual(posts,['/pgm/sync','/pgm/upload']);assert.deepEqual(stages,['checking','syncing','transferring','verifying','complete']);}
 else {assert(error);assert.notEqual(tx.state,'complete');assert.equal(posts.filter(p=>p==='/pgm/upload').length,mode==='disconnect'||mode==='bad-result'?1:0);}
 if(['unsupported','clients','queued'].includes(mode))assert.equal(posts.length,0);
 if(mode==='disconnect')assert.equal(tx.state,'unverified');
 assert(!tx.busy);
}
for(const mode of ['normal','unsupported','clients','queued','timeout','sync-fail','ack-only','disconnect','bad-result'])await run(mode);
const damaged=prepareHex(file);damaged.hex[0]=0;let touched=false;
await assert.rejects(()=>new ProgramTransaction({request(){touched=true;}}).run(damaged));assert(!touched);
console.log('PASS prepared HEX/page normalization, invalid file before reset, raw request/text handling, exclusive preflight, sync negotiation, complete readback counts and no ambiguous mutation retry');
