import assert from 'node:assert/strict';
import {parseArgs} from 'node:util';
import {writeFile} from 'node:fs/promises';
import {launch} from './headless_browser.mjs';
const {values:{base,browser,output}}=parseArgs({options:{base:{type:'string'},browser:{type:'string'},output:{type:'string'}}});
if(new URL(base).hostname!=='127.0.0.1')throw Error('Loopback fixtures only');
const page=await launch(browser,output),results=[];
const run=fn=>page.evaluate(`(${fn})()`),check=async(name,fn)=>{await fn();results.push(name);console.log('PASS '+name);};
try{
 await page.send('Page.addScriptToEvaluateOnNewDocument',{source:`const originalFetch=window.fetch.bind(window);window.reviewPosts=0;window.fetch=async(url,opts)=>{const path=new URL(url,location.href).pathname;if(opts?.method==='POST')window.reviewPosts++;if(path==='/api/v1/network'&&window.failNetwork)return new Response('{}',{status:503});const r=await originalFetch(url,opts);if(path==='/api/v1/settings'&&window.delaySettings)await new Promise(resolve=>window.releaseSettings=resolve);return r;};`});
 await page.send('Page.navigate',{url:base+'/index.html#/network'});
 await page.wait("document.querySelector('#network-ap')?.dataset.state==='clean'");
 await check('Open Wi-Fi clears hidden password drafts for both setup modes',async()=>{
  for(const kind of ['ap','station']){
   const v=await page.evaluate(`(()=>{const f=document.querySelector('#network-${kind}'),a=f.elements.password_action,p=f.elements.password,s=f.elements.security;a.value='replace';a.dispatchEvent(new Event('change',{bubbles:true}));p.value='DUMMY-REVIEW-PASSWORD';p.dispatchEvent(new Event('input',{bubbles:true}));s.value='open';s.dispatchEvent(new Event('change',{bubbles:true}));return {action:a.value,password:p.value,hidden:p.closest('.form-field').hidden,disabled:f.querySelector('[type=submit]').disabled};})()`);
   assert.deepEqual(v,{action:'clear',password:'',hidden:true,disabled:false});
  }
  await run(()=>{window.confirm=()=>true;document.querySelector('#network-reload').click();});await page.wait("document.querySelector('#network-ap')?.dataset.state==='clean'");
 });
 await check('Failed Network reload retains submit prevention and working forms',async()=>{
  await run(()=>{window.failNetwork=true;document.querySelector('#network-reload').click();});await page.wait("document.querySelector('#view').textContent.includes('Couldn’t load network settings')");
  const v=await run(()=>{const f=document.querySelector('#network-ap'),old=f.elements.ssid.value;let prevented;const disabled=f.querySelector('[type=submit]').disabled;f.elements.ssid.value='';f.addEventListener('submit',e=>{prevented=e.defaultPrevented;e.preventDefault();},{once:true});f.requestSubmit();f.elements.ssid.value=old;f.dispatchEvent(new Event('input'));return {disabled,prevented};});
  assert.deepEqual(v,{disabled:false,prevented:true});assert.equal(await run(()=>window.reviewPosts),0);
 });
 await page.send('Page.navigate',{url:base+'/index.html#/settings'});await page.wait("document.querySelector('#management-description')?.dataset.state==='clean'");
 await check('Forced Settings reload locks edits and submissions until the read completes',async()=>{
  await run(()=>{window.delaySettings=true;document.querySelector('#management-reload').click();});await page.wait("typeof window.releaseSettings==='function'");
  assert.equal(await run(()=>document.querySelector('#management-description-description').disabled),true);
  await run(()=>{const f=document.querySelector('#management-description');f.elements.description.value='PROGRAMMATIC-PROBE';f.requestSubmit();});assert.equal(await run(()=>window.reviewPosts),0);
  await run(()=>{window.delaySettings=false;window.releaseSettings();});await page.wait("!document.querySelector('#management-description-description').disabled");
  assert.notEqual(await run(()=>document.querySelector('#management-description-description').value),'PROGRAMMATIC-PROBE');
 });
 await check('Short enlarged viewport retains usable content and reachable header/menu',async()=>{
  await page.send('Emulation.setDeviceMetricsOverride',{width:360,height:360,deviceScaleFactor:1,mobile:false});await run(()=>document.documentElement.style.fontSize='200%');
  const v=await run(()=>{const h=document.querySelector('.topbar'),main=document.querySelector('main'),r=main.getBoundingClientRect();return {visible:Math.max(0,Math.min(r.bottom,innerHeight)-Math.max(0,r.top)),scrolls:main.scrollHeight>main.clientHeight,headerScrolls:h.scrollHeight>h.clientHeight};});
  assert(v.visible>100,JSON.stringify(v));assert(v.scrolls);assert(v.headerScrolls);
  await run(()=>document.querySelector('#theme').focus());await run(()=>document.querySelector('#menu').click());await page.wait("!document.querySelector('#navigation').hidden");
  assert((await run(()=>document.querySelector('main').getBoundingClientRect().height))>0);await page.key('Escape');await page.screenshot('short-viewport-fixed.png');
  await page.send('Emulation.setDeviceMetricsOverride',{width:1440,height:900,deviceScaleFactor:1,mobile:false});await run(()=>document.documentElement.style.fontSize='');
 });
 await run(()=>location.hash='#/firmware');await page.wait("document.querySelector('#view').dataset.updateState==='ready'");
 await check('Oversized firmware is rejected before File.arrayBuffer is called',async()=>{
  await run(()=>{window.fileReads=0;File.prototype.arrayBuffer=function(){window.fileReads++;throw Error('Unexpected whole-file read');};const d=new DataTransfer();d.items.add(new File([new Uint8Array(1048576)],'too-large.bin'));const input=document.querySelector('#update-files');input.files=d.files;input.dispatchEvent(new Event('change'));});
  await page.wait("document.querySelector('#update-file-state').textContent.includes('between 1024')");assert.equal(await run(()=>window.fileReads),0);assert.equal(await run(()=>window.reviewPosts),0);
 });
 const callback={message:'<b>Ready</b>',items:['one','two'],rows:[['Heading'],['Value']],value:'sample',enabled:true};let buttonId=null;
 await page.send('Fetch.enable',{patterns:[{urlPattern:'*/user/probe.html.json*',requestStage:'Request'}]});
 page.on('Fetch.requestPaused',p=>{const url=new URL(p.request.url);if(p.request.method==='POST')buttonId=url.searchParams.get('id');page.send('Fetch.fulfillRequest',{requestId:p.requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:Buffer.from(JSON.stringify(callback)).toString('base64')}).catch(()=>{});});
 await page.send('Page.navigate',{url:base+'/user/probe.html'});
 await check('Minified custom-page helpers preserve rendering, form values, public globals and exact button IDs',async()=>{
  await page.wait("document.querySelector('#message b')?.textContent==='Ready'");
  assert.equal(await run(()=>document.querySelectorAll('#items li').length),2);assert.equal(await run(()=>document.querySelectorAll('#rows tr').length),2);assert.equal(await run(()=>document.querySelector('[name=value]').value),'sample');assert.equal(await run(()=>document.querySelector('[name=enabled]').checked),true);
  assert.equal(await run(()=>['ajaxJson','notifyResponse','notifyButtonPressed','showWarning','onLoad'].every(k=>typeof window[k]==='function')),true);
  await run(()=>document.getElementById('pump&fan"quoted').click());await page.wait("document.querySelector('#message b')?.textContent==='Ready'");
  for(let i=0;i<100&&buttonId===null;i++)await page.evaluate('new Promise(r=>setTimeout(r,20))');assert.equal(buttonId,'pump&fan"quoted');
  for(const width of [1440,360]){await page.send('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:false});await page.wait('document.documentElement.scrollWidth<=innerWidth');await page.screenshot('custom-page-'+width+'.png');}
 });
 await writeFile(output+'/result.json',JSON.stringify({pass:true,results},null,2));
}catch(error){await page.screenshot('failure.png');await writeFile(output+'/failure-dom.html',await page.evaluate('document.documentElement.outerHTML'));throw error;}finally{await page.close();}
