import {testCustomPagesReload} from './test_custom_pages_browser.mjs';
import {testSerialRecovery} from './test_serial_browser.mjs';
import {testNetworkRecovery,testNetworkReload} from './test_network_browser.mjs';
import {testManagementRecovery,testManagementReload} from './test_management_browser.mjs';
import {testUpdatesRecovery,testUpdatesReload} from './test_updates_browser.mjs';
import {testShellRecovery} from './test_shell_recovery.mjs';
import assert from 'node:assert/strict';
import {readFile,writeFile} from 'node:fs/promises';
import {parseArgs} from 'node:util';
import {launch,sleep} from '../support/headless_browser.mjs';
const {values:{base,browser,output}}=parseArgs({options:{base:{type:'string'},browser:{type:'string'},output:{type:'string'}}});
assert.equal(new URL(base).hostname,'127.0.0.1');
const fixtures=JSON.parse(await readFile(new URL('../support/fixtures/api-v1.json',import.meta.url)));
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
 const mockScript=await page.send('Page.addScriptToEvaluateOnNewDocument',{source:'const FIXTURES='+JSON.stringify(fixtures)+';('+mock.toString()+')();'});
 const context={page,run,check,fresh,hold,release,count,stage};
 for(const test of [testSerialRecovery,testNetworkRecovery,testManagementRecovery,testUpdatesRecovery,testShellRecovery])await test(context);
 await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:mockScript.identifier});
 await page.send('Page.addScriptToEvaluateOnNewDocument',{source:`const originalFetch=window.fetch.bind(window);window.reviewPosts=0;window.fetch=async(url,opts)=>{const path=new URL(url,location.href).pathname;if(opts?.method==='POST')window.reviewPosts++;if(path==='/api/v1/network'&&window.failNetwork)return new Response('{}',{status:503});const r=await originalFetch(url,opts);if(path==='/api/v1/settings'&&window.delaySettings)await new Promise(resolve=>window.releaseSettings=resolve);return r;};`});
 for(const test of [testNetworkReload,testManagementReload,testUpdatesReload,testCustomPagesReload])await test({page,run,check,base});
 assert.deepEqual(errors,[]);
 await writeFile(output+'/result.json',JSON.stringify({pass:true,results},null,2));
}catch(error){await page.screenshot('failure.png');await writeFile(output+'/failure-dom.html',await page.evaluate('document.documentElement.outerHTML'));throw error;}finally{await page.close();}
