import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {parseArgs} from 'node:util';
import {launch,sleep} from './headless_browser.mjs';
const {values:{base,browser,output}}=parseArgs({options:{base:{type:'string'},browser:{type:'string'},output:{type:'string'}}});
assert.equal(new URL(base).hostname,'127.0.0.1');
const fixtures=JSON.parse(await readFile(new URL('./fixtures/api-v1.json',import.meta.url)));
const page=await launch(browser,output),results=[],errors=[];
page.on('Runtime.exceptionThrown',p=>errors.push(p.exceptionDetails.exception?.description||p.exceptionDetails.text));
const run=fn=>page.evaluate(`(${fn})()`),check=async(name,fn)=>{await fn();results.push(name);console.log('PASS '+name);};
function mock(){
 const real=fetch.bind(window),t=window.t={data:FIXTURES,calls:[],holds:{},pending:{},mode:'off',logReads:0};
 t.data['/api/v1/serial'].clients=1;
 t.data['/api/v1/firmware'].ota_supported=true;t.data['/api/v1/firmware'].reason=null;
 const response=(v,status=200)=>new Response(typeof v==='string'?v:JSON.stringify(v),{status,headers:{'Content-Type':'application/json'}});
 t.wait=key=>new Promise(resolve=>t.pending[key]=resolve);
 window.confirm=()=>true;
 window.fetch=async(url,options={})=>{
  const path=new URL(url,location.href).pathname,method=options.method||'GET',key=method+' '+path;
  t.calls.push(key);
  if(t.holds[key])await t.wait(key);
  if(options.signal?.aborted)throw new DOMException('Aborted','AbortError');
  if(path==='/api/v1/diagnostics/uart-log'){
   if(method==='POST')t.mode=new URLSearchParams(options.body).get('mode');
   return response({schema:1,mode:t.mode,uart_enabled:t.mode!=='off'});
  }
  if(path==='/api/v1/diagnostics/log'){t.logReads++;return response({schema:1,boot:42,epoch:0,start:0,end:0,reason:null,lost:0,encoding:'base64',data:''});}
  if(path==='/api/v1/serial/reset-mcu'||path==='/api/v1/serial/clear')return response(t.data['/api/v1/serial']);
  if(path==='/api/v1/serial/send')return response({schema:1,accepted_bytes:new URLSearchParams(options.body).get('hex').length/2});
  if(path==='/api/v1/network/scan')return response({schema:1,state:'complete',total:0,retained:0,page:0,truncated:false,networks:[]});
  if(path.startsWith('/api/v1/network/')&&method==='POST')return response({id:0,state:'idle',error:null});
  if(path==='/api/v1/settings/restart'){
   t.data['/api/v1/status'].system.uptime_ms=1;
   if(t.dropRestart)throw new TypeError('Lost restart reply');
   return response({accepted:true});
  }
  if(path==='/api/v1/firmware/activate'){
   t.data['/api/v1/status'].firmware.slot='user2';t.data['/api/v1/status'].firmware.version='new-version';
   t.data['/api/v1/status'].system.uptime_ms=1;
   if(t.dropActivation)throw new TypeError('Lost activation reply');
   return response('');
  }
  if(t.data[path])return response(t.data[path]);
  return real(url,options);
 };
 window.XMLHttpRequest=class {
  constructor(){this.upload={};}
  open(method,path){this.key=method+' '+path;}
  setRequestHeader(){}
  abort(){this.aborted=true;this.onabort?.();}
  async send(){t.calls.push(this.key);if(t.holds[this.key])await t.wait(this.key);if(this.aborted)return;if(t.failUpload){this.onerror?.();return;}this.status=204;this.responseText='';this.onload?.();}
 };
}
let navigation=0;
async function fresh(route){await page.send('Page.navigate',{url:base+'/index.html?lifecycle='+(++navigation)+'#/'+route});await page.wait("document.body?.dataset.ready==='true'");}
async function hold(key){await page.evaluate(`t.holds[${JSON.stringify(key)}]=true`);}
async function release(key){await page.evaluate(`delete t.holds[${JSON.stringify(key)}];t.pending[${JSON.stringify(key)}]?.()`);}
async function count(key){return page.evaluate(`t.calls.filter(k=>k===${JSON.stringify(key)}).length`);}
async function stage(){await run(()=>{const b=new Uint8Array(1028),v=new DataView(b.buffer);b.set([0xea,4,0,1]);v.setUint32(4,0x40100000,true);v.setUint32(12,16,true);b.set(new TextEncoder().encode(t.data['/api/v1/firmware'].image_abi),32);const d=new DataTransfer();d.items.add(new File([b],'user2.bin'));const f=document.querySelector('#update-files');f.files=d.files;f.dispatchEvent(new Event('change'));});await page.wait("document.querySelector('#update-file-state').textContent.includes('checks passed')");await run(()=>{const c=document.querySelector('#update-replace-confirm');c.checked=true;c.dispatchEvent(new Event('change'));});}
try{
 await page.send('Network.enable');await page.send('Network.setCacheDisabled',{cacheDisabled:true});
 await page.send('Page.addScriptToEvaluateOnNewDocument',{source:'const FIXTURES='+JSON.stringify(fixtures)+';('+mock.toString()+')();'});
 await fresh('serial');await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean'");
 await check('Serial reset owns mutation controls across refresh and cancels queued work on departure',async()=>{
  const key='POST /api/v1/serial/reset-mcu';await hold(key);
  await run(()=>document.querySelector('#serial-mcu-tools').open=true);
  await run(()=>[...document.querySelectorAll('button')].find(b=>b.textContent==='Reset attached MCU').click());await page.wait("!!t.pending['POST /api/v1/serial/reset-mcu']");
  assert.equal(await run(()=>document.querySelector('#serial-uart [type=submit]').disabled),true);
  assert.equal(await run(()=>[...document.querySelectorAll('button')].find(b=>b.textContent==='Clear device capture').disabled),true);
  await run(()=>document.querySelector('#refresh').click());await sleep(30);
  assert.equal(await count(key),1);
  await run(()=>location.hash='#/network');await release(key);await page.wait("document.querySelector('#network-ap')?.dataset.state==='clean'");
  assert.equal(await run(()=>document.querySelector('#title').textContent),'Network');
 });
 await check('Pending scan cannot overwrite a save; invalid receipt ends in unverified',async()=>{
  const key='POST /api/v1/network/scan';await hold(key);
  await run(()=>{document.querySelector('#network-tab-station').click();document.querySelector('[data-scan]').click();});await page.wait("!!t.pending['POST /api/v1/network/scan']");
  await run(()=>{document.querySelector('#network-tab-ap').click();document.querySelector('#network-ap').requestSubmit();});await release(key);
  await page.wait("document.querySelector('#network-stage')?.dataset.state==='unverified'");
  assert.equal(await run(()=>document.querySelector('#network-stage').dataset.state),'unverified');
  assert.equal(await count('POST /api/v1/network/ap'),1);
  assert.equal(await run(()=>document.querySelector('[data-scan]').disabled),false);
 });
 await fresh('diagnostics');await page.wait("document.querySelector('[data-mode=off]')?.disabled===false");
 await check('Debug UART refresh excludes writes and keeps controller ownership',async()=>{
  const key='GET /api/v1/diagnostics/uart-log';await hold(key);await run(()=>document.querySelector('#refresh').click());await page.wait("!!t.pending['GET /api/v1/diagnostics/uart-log']");
  assert.equal(await run(()=>[...document.querySelectorAll('[data-mode]')].every(b=>b.disabled)),true);
  await run(()=>document.querySelector('[data-mode=on0]').dispatchEvent(new Event('click')));
  assert.equal(await count('POST /api/v1/diagnostics/uart-log'),0);
  await release(key);await page.wait("!document.querySelector('[data-mode=off]').disabled");
  await run(()=>document.querySelector('[data-mode=on0]').click());await page.wait("document.querySelector('[data-mode=on0]').getAttribute('aria-pressed')==='true'");
  assert.equal(await count('POST /api/v1/diagnostics/uart-log'),1);
 });
 await check('Repeated debug-log refreshes retain one timer chain',async()=>{
  for(let i=0;i<3;i++){await run(()=>document.querySelector('#refresh').click());await sleep(60);}
  const before=await run(()=>t.logReads);await sleep(1800);
  assert.equal((await run(()=>t.logReads))-before,1);
 });
 await fresh('settings');await page.wait("document.querySelector('#management-description')?.dataset.state==='clean'");
 await check('Lost restart reply reconciles from observed uptime without a second POST',async()=>{
  await run(()=>{t.dropRestart=true;[...document.querySelectorAll('button')].find(b=>b.textContent==='Restart ESP Serial2WiFi').click();});await page.wait("document.querySelector('#view').textContent.includes('Checking whether')");
  await run(()=>document.querySelector('#refresh').click());await page.wait("document.querySelector('#view').textContent.includes('restarted and is responding')");
  assert.equal(await count('POST /api/v1/settings/restart'),1);
  assert.equal(await run(()=>[...document.querySelectorAll('button')].find(b=>b.textContent==='Restart ESP Serial2WiFi').disabled),false);
 });
 await fresh('firmware');await page.wait("document.querySelector('#view').dataset.updateState==='ready'");
 await check('Replacement upload failure hides the prior activation action',async()=>{
  await stage();await run(()=>document.querySelector('#update-form').requestSubmit());await page.wait("document.querySelector('#view').dataset.updateState==='uploaded'");
  assert.equal(await run(()=>document.querySelector('#update-activate').hidden),false);
  await run(()=>{t.failUpload=true;document.querySelector('#update-form').requestSubmit();});await page.wait("document.querySelector('#view').dataset.updateState==='unverified'");
  assert.equal(await run(()=>document.querySelector('#update-activate').hidden),true);
 });
 await check('Lost activation reply verifies boot and no-version completion ends pending state',async()=>{
  await run(()=>{t.failUpload=false;document.querySelector('#update-form').requestSubmit();});await page.wait("document.querySelector('#view').dataset.updateState==='uploaded'");
  await run(()=>{t.dropActivation=true;document.querySelector('#update-activate').click();});
  await page.wait("document.querySelector('#update-state').textContent.includes('new slot is running')");
  assert.equal(await count('POST /api/v1/firmware/activate'),1);
  assert.equal(await run(()=>document.querySelector('#update-activate').hidden),true);
  assert.equal(await run(()=>document.querySelector('#update-version').disabled),false);
  assert.equal(await run(()=>document.querySelector('#update-files').value),'');
 });
 await fresh('firmware');await page.wait("document.querySelector('#view').dataset.updateState==='ready'");
 await check('Slot changes invalidate a validated file and confirmation',async()=>{
  await stage();await run(()=>{t.data['/api/v1/firmware'].active_slot=2;t.data['/api/v1/firmware'].target_slot=1;document.querySelector('#update-reload').click();});
  await page.wait("document.querySelector('#update-file-state').textContent.includes('Select the files again')");
  assert.equal(await run(()=>document.querySelector('#update-form [type=submit]').disabled),true);
 });
 await fresh('firmware');await page.wait("document.querySelector('#view').dataset.updateState==='ready'");
 await check('Busy update survives BFCache restore without background reads or feature retries',async()=>{
  await stage();const key='POST /api/v1/firmware/upload';await hold(key);await run(()=>document.querySelector('#update-form').requestSubmit());await page.wait("!!t.pending['POST /api/v1/firmware/upload']");
  const before=await run(()=>t.calls.length);
  await run(()=>{dispatchEvent(new PageTransitionEvent('pagehide',{persisted:true}));dispatchEvent(new PageTransitionEvent('pageshow',{persisted:true}));document.querySelector('#view').dispatchEvent(new CustomEvent('device-operation',{bubbles:true,detail:{busy:false,owner:{}}}));document.querySelector('#retry-features').click();});await sleep(50);
  assert.equal(await run(()=>t.calls.length),before);
  assert.equal(await run(()=>document.querySelector('#refresh').disabled),true);
  await release(key);await page.wait("document.querySelector('#view').dataset.updateState==='uploaded'");
 });
 await check('Pre-reboot capability work is cancelled or discarded and fresh features replace it',async()=>{
  await fresh('dashboard');await page.wait("!document.querySelector('#refresh').disabled && !document.querySelector('#retry-features').disabled");
  const before=await count('GET /api/v1/capabilities'),key='GET /api/v1/status';await hold(key);
  await run(()=>document.querySelector('#refresh').click());await page.wait("!!t.pending['GET /api/v1/status']");
  await run(()=>{document.querySelector('#retry-features').click();t.data['/api/v1/status'].firmware.version='changed-build';t.data['/api/v1/capabilities'].features.custom_pages=false;});
  assert.equal(await count('GET /api/v1/capabilities'),before);
  await release(key);await page.wait("document.querySelector('[data-feature=custom_pages]').hidden && !document.querySelector('#retry-features').disabled");
  const reads=await count('GET /api/v1/capabilities');assert(reads>=before+1&&reads<=before+2,'Expected one fresh read plus at most one old admitted read');
 });
 await check('Leaving Serial cancels a queued reset before transport dispatch',async()=>{
  await fresh('serial');await page.wait("document.querySelector('#serial-uart')?.dataset.state==='clean' && !document.querySelector('#refresh').disabled");
  const key='GET /api/v1/status';await hold(key);await run(()=>document.querySelector('#refresh').click());await page.wait("!!t.pending['GET /api/v1/status']");
  await run(()=>{[...document.querySelectorAll('button')].find(b=>b.textContent==='Reset attached MCU').click();location.hash='#/dashboard';});
  await page.wait("document.querySelector('#title').textContent==='Device overview'");await release(key);await sleep(50);
  assert.equal(await count('POST /api/v1/serial/reset-mcu'),0);
 });
 await check('Destroyed DraftForm cannot read after write or restart itself',async()=>{
  const result=await page.evaluate(`(async()=>{const {DraftForm}=await import('/__test/core.js');const f=document.createElement('form');f.innerHTML='<input name="value"><button type="submit">Save</button>';let reads=0,release;const d=new DraftForm(f,{read:async()=>{reads++;return {value:'a'};},write:()=>new Promise(r=>release=r)});await d.load();f.elements.value.value='b';const save=d.save();d.destroy();release({});await save;await d.load();await d.save();d.reset();return {reads,baseline:d.baseline};})()`);
  assert.deepEqual(result,{reads:1,baseline:null});
 });
 // Cause one real stylesheet load failure, then retry by remounting the failed instance.
 await page.send('Fetch.enable',{patterns:[{urlPattern:'*xterm.css*',requestStage:'Request'}]});let failCSS=true;
 page.on('Fetch.requestPaused',p=>{page.send(failCSS?'Fetch.failRequest':'Fetch.continueRequest',failCSS?{requestId:p.requestId,errorReason:'Failed'}:{requestId:p.requestId}).catch(()=>{});failCSS=false;});
 await fresh('serial');
 await check('Failed terminal initialization recovers on remount',async()=>{
  await page.wait("document.querySelector('.live-console')?.dataset.state==='failed'");
  await run(()=>location.hash='#/dashboard');await page.wait("document.querySelector('#title').textContent==='Device overview'");
  await run(()=>location.hash='#/serial');await page.wait("!!document.querySelector('#serial-terminal textarea')");
  assert.notEqual(await run(()=>document.querySelector('.live-console').dataset.state),'failed');
 });
 await page.send('Fetch.disable');
 assert.deepEqual(errors,[]);
 await writeFile(output+'/result.json',JSON.stringify({pass:true,results},null,2));
}catch(error){await page.screenshot('failure.png');await writeFile(output+'/failure-dom.html',await page.evaluate('document.documentElement.outerHTML'));throw error;}finally{await page.close();}
