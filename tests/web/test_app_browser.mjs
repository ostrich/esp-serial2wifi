import {testBrandingBrowser} from './test_branding_browser.mjs';
import {testUpdatesBrowser} from './test_updates_browser.mjs';
import {testManagementBrowser} from './test_management_browser.mjs';
import {testSerialBrowser} from './test_serial_browser.mjs';
import {testNetworkBrowser} from './test_network_browser.mjs';
import assert from 'node:assert/strict';
import {mkdir, writeFile} from 'node:fs/promises';
import {parseArgs} from 'node:util';
import {join} from 'node:path';
import {launch,sleep} from '../support/headless_browser.mjs';
const {values} = parseArgs({options:{base:{type:'string'},browser:{type:'string'},output:{type:'string'},suite:{type:'string',default:'all'}}});
const {base,browser,output}=values;
if (!new URL(base).hostname.match(/^(127\.0\.0\.1|localhost)$/)) throw new Error('Fixtures must be local');
await mkdir(output,{recursive:true});
const page=await launch(browser,output), results=[];
const run=fn=>page.evaluate(`(${fn})()`);
const check=async(name,fn)=>{await fn();results.push(name);console.log('PASS',name);};
const scenario=async (mode,route='/api/v1/status',response)=>{
 const config={route,mode,delay:.3};if(response!==undefined)config.response=response;
 const r=await fetch(base+'/__test/scenario',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify(config)});assert.equal(r.status,200);
};
const refresh=()=>page.evaluate("document.querySelector('#refresh').click()");
const noOverflow=async()=>assert.equal(await run(()=>document.documentElement.scrollWidth<=innerWidth),true);
const viewport=async width=>{
 await page.send('Emulation.setDeviceMetricsOverride',{width,height:900,deviceScaleFactor:1,mobile:width===360});
 // Let media-query change handlers and layout finish before focusing or typing.
 await run(()=>new Promise(resolve=>requestAnimationFrame(()=>requestAnimationFrame(resolve))));
};
const exceptions=[];page.on('Runtime.exceptionThrown',params=>exceptions.push(params.exceptionDetails.text));
try {
 if(['management','updates','serial','network','branding'].includes(values.suite)){
  await viewport(1440);await page.send('Page.navigate',{url:base+'/index.html'});
  await page.wait("document.body?.dataset.ready==='true'");
  await (values.suite==='branding'?testBrandingBrowser:values.suite==='updates'?testUpdatesBrowser:values.suite==='serial'?testSerialBrowser:values.suite==='network'?testNetworkBrowser:testManagementBrowser)({page,run,check,viewport,noOverflow});
 }else{
 await viewport(1440);await page.send('Page.navigate',{url:base+'/index.html'});
 await page.wait("document.body?.dataset.ready==='true' && document.querySelector('#connection')?.dataset.state==='connected'");
 await check('Independent shell, truthful identity and local assets',async()=>{
  assert.equal(await run(()=>document.querySelector('#device-name').textContent),'recovery-demo');
  assert.equal(await run(()=>document.querySelector('#device-address').textContent),'192.168.4.1');
  assert.equal(await run(()=>document.querySelector('[data-field=address]').textContent),new URL(base).host);
  assert((await run(()=>document.querySelector('[data-field=address]').previousElementSibling.textContent))==='Web interface address');
  assert.equal(await run(()=>document.querySelector('nav [aria-current=page]').textContent),'Dashboard');
  assert.equal(await run(()=>performance.getEntriesByType('resource').some(entry=>/\/ui\.js(?:$|\?)/.test(entry.name))),false);
  assert.equal(await run(()=>performance.getEntriesByType('resource').every(entry=>new URL(entry.name).origin===location.origin)),true);
  assert.equal(await run(()=>document.querySelectorAll('a[aria-disabled=true]').length),0);
 });
 await check('Keyboard menu, navigation focus and Escape recovery',async()=>{
  await viewport(360);await run(()=>document.querySelector('#menu').focus());await page.key('Enter');
  assert.equal(await run(()=>document.querySelector('#menu').getAttribute('aria-expanded')),'true');
  await page.key('Tab');assert.equal(await run(()=>document.activeElement.textContent),'Dashboard');
  await page.key('Tab');assert.equal(await run(()=>document.activeElement.textContent),'Network');
  await page.key('Tab');assert.equal(await run(()=>document.activeElement.textContent),'Serial & MCU');
  await page.key('Tab');assert.equal(await run(()=>document.activeElement.textContent),'Integrations');
  await page.key('Tab');assert.equal(await run(()=>document.activeElement.textContent),'Diagnostics');
  await page.key('Enter');await page.wait("location.hash==='#/diagnostics' && document.activeElement.id==='main'");
  assert.equal(await run(()=>document.querySelector('nav[hidden]')!==null),true);
  await run(()=>document.querySelector('#menu').focus());await page.key('Enter');await page.key('Tab');await page.key('Escape');
  assert.equal(await run(()=>document.activeElement.id),'menu');
  assert.equal(await run(()=>document.querySelector('#menu').getAttribute('aria-expanded')),'false');
  await run(()=>document.querySelector('.skip').focus());await page.key('Enter');
  assert.equal(await run(()=>location.hash),'#/diagnostics');
  assert.equal(await run(()=>document.activeElement.id),'main');
 });
 await check('Light/dark contrast, text scaling and color-independent status',async()=>{
  const contrast=await run(()=>{
   const luminance=hex=>{hex=hex.trim().slice(1);if(hex.length===3)hex=[...hex].map(x=>x+x).join('');const rgb=hex.match(/../g).map(x=>parseInt(x,16)/255).map(x=>x<=.04045?x/12.92:((x+.055)/1.055)**2.4);return rgb.reduce((v,x,i)=>v+x*[.2126,.7152,.0722][i],0);};
   const ratios=[];
   for (const theme of ['light','dark']) {
    document.documentElement.dataset.theme=theme;const style=getComputedStyle(document.documentElement);
    for (const [a,b] of [['ink','canvas'],['muted','canvas'],['muted','surface'],['action','selected'],['action-ink','action'],['error','error-bg'],['warning','warning-bg'],['success','canvas'],['success','surface']]) {
     const x=luminance(style.getPropertyValue('--'+a)),y=luminance(style.getPropertyValue('--'+b));ratios.push({theme,a,b,ratio:(Math.max(x,y)+.05)/(Math.min(x,y)+.05)});
    }
   }return ratios;
  });
  assert.ok(contrast.every(row=>row.ratio>=4.5),JSON.stringify(contrast));
  await viewport(360);await run(()=>document.documentElement.style.fontSize='200%');await noOverflow();
  await page.screenshot('large-text-360.png');await run(()=>document.documentElement.style.fontSize='');
  await viewport(1440);await page.send('Emulation.setEmulatedVisionDeficiency',{type:'deuteranopia'});
  assert.ok((await run(()=>document.querySelector('#connection').textContent)).includes('Device reachable'));
  await page.screenshot('dark-deuteranopia.png');await page.send('Emulation.setEmulatedVisionDeficiency',{type:'none'});
  await run(()=>document.documentElement.dataset.theme='light');
  await writeFile(join(output,'contrast.json'),JSON.stringify(contrast,null,2)+'\n');
 });
 await check('HTTP/malformed/disconnect states retain stale values and recover',async()=>{
  for (const mode of ['http-error','malformed','disconnect']) {
   await scenario(mode);await refresh();await page.wait("!document.querySelector('#error').hidden && !document.querySelector('#retry').disabled");
   assert.equal(await run(()=>document.querySelector('#device-name').textContent),'recovery-demo');
   assert.equal(await run(()=>document.querySelector('#connection').dataset.state),'degraded');
   assert.ok((await run(()=>document.querySelector('#updated').textContent)).includes('waiting for a new update'));
   await scenario('normal');await run(()=>document.querySelector('#retry').click());
   await page.wait("document.querySelector('#error').hidden && !document.querySelector('#refresh').disabled");
  }
 });
 await check('Real Fetch timeout and cancellation have distinct terminal results',async()=>{
  await scenario('delayed');
  const outcomes=await run(async()=>{
   const {ApiClient}=await import('/__test/core.js'),api=new ApiClient();
   const outcomes=[];
   try{await api.request('/api/v1/status',{timeout:30,retries:0});}catch(error){outcomes.push(error.kind);}
   const controller=new AbortController(),request=api.request('/api/v1/status',{signal:controller.signal,retries:0});
   setTimeout(()=>controller.abort(),10);try{await request;}catch(error){outcomes.push(error.kind);}
   return {outcomes,pending:api.pending.size,active:api.active};
  });
  assert.deepEqual(outcomes,{outcomes:['timeout','abort'],pending:0,active:0});await scenario('normal');
 });
 await check('Untrusted identity remains text, including long markup-like strings',async()=>{
  const attack='long-'.repeat(45)+'<img src=x onerror="window.__xss=1"> & 雪';
  const fixture=await(await fetch(base+'/api/v1/status')).json();fixture.hostname=attack;fixture.description=attack;
  await page.send('Fetch.enable',{patterns:[{urlPattern:'*/api/v1/status',requestStage:'Request'}]});
  let error;const off=page.on('Fetch.requestPaused',params=>{
   page.send('Fetch.fulfillRequest',{requestId:params.requestId,responseCode:200,responseHeaders:[{name:'Content-Type',value:'application/json'}],body:Buffer.from(JSON.stringify(fixture)).toString('base64')}).catch(e=>{error=e;});
  });
  await refresh();await page.wait(`document.querySelector('#device-name').textContent===${JSON.stringify(attack)}`);
  assert.equal(await run(()=>Boolean(window.__xss)),false);assert.equal(await run(()=>document.querySelectorAll('#view img').length),0);
  await viewport(360);await noOverflow();await page.screenshot('long-identity-360.png');
  off();await page.send('Fetch.disable');if(error)throw error;
  await refresh();await page.wait("document.querySelector('#device-name').textContent==='recovery-demo'");
 });
 await check('Compiled features, broker acceptance and recovery AP are represented truthfully',async()=>{
  const normal=await(await fetch(base+'/api/v1/status')).json();
  const caps=await(await fetch(base+'/api/v1/capabilities')).json();
  const minimal=structuredClone(caps);
  for(const key of ['mqtt','custom_pages','rest','socket'])minimal.features[key]=false;
  const noMqtt=structuredClone(normal);delete noMqtt.mqtt;
  await scenario('normal','/api/v1/capabilities',minimal);await scenario('normal','/api/v1/status',noMqtt);
  await page.send('Page.navigate',{url:base+'/index.html#/dashboard'});
  // CDP navigation completion does not mean a same-document hash navigation
  // has committed. Reloading immediately can reload the prior Diagnostics URL.
  await page.wait("location.hash==='#/dashboard' && document.title==='ESP Serial2WiFi · Device overview'");
  await page.send('Page.reload',{ignoreCache:true});
  await page.wait("document.body?.dataset.ready==='true' && document.querySelector('[data-card=mqtt]')?.hidden && !document.querySelector('#retry-features').disabled");
  assert.equal(await run(()=>document.querySelector('[data-feature=integrations]').hidden),true);
  assert.equal(await run(()=>document.querySelector('[data-feature=custom_pages]').hidden),true);
  const pagesOnly=structuredClone(minimal);pagesOnly.features.custom_pages=true;
  await scenario('normal','/api/v1/capabilities',pagesOnly);await run(()=>document.querySelector('#retry-features').click());
  await page.wait("!document.querySelector('[data-feature=integrations]').hidden && !document.querySelector('[data-feature=custom_pages]').hidden");
  assert.equal(await run(()=>document.querySelector('[data-card=mqtt]').hidden),true);
  await scenario('normal','/api/v1/capabilities');
  const connecting=structuredClone(normal);connecting.mqtt={enabled:true,state:'connecting'};
  await scenario('normal','/api/v1/status',connecting);await page.send('Page.reload',{ignoreCache:true});
  await page.wait("document.querySelector('[data-card=mqtt] [data-part=badge]')?.textContent==='Connecting to broker'");
  for(const [state,label] of [['failed','Failed'],['connected','Broker connected'],['disabled','Disabled']]){
   const value=structuredClone(normal);value.mqtt={enabled:state!=='disabled',state};
   await scenario('normal','/api/v1/status',value);await refresh();
   await page.wait('document.querySelector("[data-card=mqtt] [data-part=badge]").textContent==='+JSON.stringify(label));
   assert.equal(await run(()=>document.querySelector('[data-card=mqtt] [data-part=badge]').dataset.state),state);
  }
  const recovery=structuredClone(normal);recovery.wifi.mode='station_ap';recovery.wifi.state='failed';
  recovery.wifi.reason='wrong_password';recovery.wifi.ip=null;
  await scenario('normal','/api/v1/status',recovery);await refresh();
  await page.wait("document.querySelector('[data-card=wifi] [data-part=badge]').textContent==='Recovery AP available'");
  assert.equal(await run(()=>document.querySelector('[data-card=wifi] [data-part=badge]').dataset.state),'degraded');
  assert.equal(await run(()=>document.querySelector('#connection').dataset.state),'connected');
  await scenario('normal');await refresh();
  await page.wait("document.querySelector('[data-card=wifi] [data-part=badge]').textContent==='Access point ready'");
 });
 await check('Polling keeps action focus and expanded details; failures mark every card stale',async()=>{
  await run(()=>{document.querySelector('.advanced').open=true;window.__focusedAction=document.querySelector('.quick-actions a');window.__focusedAction.focus();});
  await refresh();await page.wait("!document.querySelector('#refresh').disabled");
  assert.equal(await run(()=>document.activeElement===window.__focusedAction&&window.__focusedAction.isConnected),true);
  assert.equal(await run(()=>document.querySelector('.advanced').open),true);
  await scenario('http-error');await refresh();await page.wait("!document.querySelector('#error').hidden && !document.querySelector('#retry').disabled");
  assert.equal(await run(()=>[...document.querySelectorAll('[data-card]:not([hidden]) [data-part=badge]')].every(node=>node.textContent.startsWith('Out of date'))),true);
  assert.equal(await run(()=>document.querySelector('.advanced').open),true);
  await scenario('normal');await run(()=>document.querySelector('#retry').click());
  await page.wait("document.querySelector('#error').hidden && !document.querySelector('#refresh').disabled");
 });
 await check('Capability failure is independent of a reachable status snapshot',async()=>{
  await scenario('http-error','/api/v1/capabilities');await page.send('Page.reload',{ignoreCache:true});
  await page.wait("document.querySelector('#connection')?.dataset.state==='connected' && document.querySelector('#cap-error') && !document.querySelector('#cap-error').hidden && !document.querySelector('#retry-features').disabled");
  assert.equal(await run(()=>document.querySelector('#error').hidden),true);
  assert.equal(await run(()=>document.querySelector('[data-card=mqtt]').hidden),true);
  await scenario('normal','/api/v1/capabilities');await run(()=>document.querySelector('#retry-features').click());
  await page.wait("document.querySelector('#cap-error').hidden && !document.querySelector('[data-card=mqtt]').hidden");
 });
 await check('Form drafts, duplicate submit, validation, authoritative verification and isolation',async()=>{
  await run(async()=>{
   const {DraftForm,ApiError,Notice}=await import('/__test/core.js');
   const host=document.createElement('section');host.id='form-test';
   host.innerHTML='<form id="one"><fieldset><legend>Identity</legend><label for="test-name">Hostname</label><input id="test-name" name="name" aria-describedby="name-error"><p id="name-error" data-error="name" hidden></p></fieldset><p data-message role="status"></p><button type="submit">Save changes</button><button type="button" data-cancel>Cancel</button></form><form id="two"><label for="other-name">Other name</label><input id="other-name" name="name"><label for="test-password">New password</label><input id="test-password" type="password" name="password" autocomplete="new-password"><button type="submit">Save other settings</button><button type="button" data-cancel>Cancel</button><p data-message></p></form><div id="notice-test" role="status" hidden></div>';
   document.querySelector('#main').append(host);
   const state=window.__formTest={value:'saved',writes:0,states:[]};
   state.first=new DraftForm(host.querySelector('#one'),{message:host.querySelector('#one [data-message]'),
    read:async()=>({name:state.value,ignored:'not a form field'}),
    write:values=>{state.writes++;return new Promise(resolve=>{state.finish=()=>{state.value=values.name;resolve();};});},
    validate:values=>values.name.trim()?{}:{name:'Enter a hostname.'}});
   state.first.addEventListener('state',event=>state.states.push(event.detail.state));
   state.second=new DraftForm(host.querySelector('#two'),{read:async()=>({name:'other',password:'DO-NOT-PREFILL'}),write:async()=>{throw new ApiError('timeout',0,true);},message:host.querySelector('#two [data-message]')});
   state.notice=new Notice(host.querySelector('#notice-test'));
   await Promise.all([state.first.load(),state.second.load()]);
  });
  assert.equal(await run(()=>document.querySelector('#test-password').value),'');
  assert.equal(await run(()=>document.querySelector('#one [type=submit]').disabled),true);
  await run(()=>{const input=document.querySelector('#test-name');input.value='A&B +% "雪"';input.dispatchEvent(new Event('input',{bubbles:true}));input.dispatchEvent(new Event('blur'));input.focus();});
  assert.equal(await run(()=>window.__formTest.writes),0);
  assert.equal(await run(()=>window.__formTest.first.canLeave(()=>false)),false);
  await page.key('Enter');await run(()=>document.querySelector('#one').requestSubmit());
  assert.equal(await run(()=>window.__formTest.writes),1);
  assert.equal(await run(()=>window.__formTest.first.state),'saving');
  await run(()=>window.__formTest.finish());await page.wait("window.__formTest.first.state==='saved'");
  assert.equal(await run(()=>window.__formTest.first.dirty),false);
  assert.equal(await run(()=>document.querySelector('#other-name').value),'other');
  await run(()=>{const s=window.__formTest;s.first.write=async()=>{s.writes++;throw new Error('never show raw server data');};const input=document.querySelector('#test-name');input.value='keep this draft';input.dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('#one').requestSubmit();});
  await page.wait("window.__formTest.first.state==='dirty' && document.querySelector('#one [data-message]').textContent.includes('edits are still here')");
  assert.equal(await run(()=>document.querySelector('#test-name').value),'keep this draft');
  await run(()=>{const s=window.__formTest;s.first.write=async()=>{s.writes++;};document.querySelector('#one').requestSubmit();});
  await page.wait("window.__formTest.first.state==='dirty' && !window.__formTest.first.busy");
  assert.equal(await run(()=>document.querySelector('#test-name').value),'keep this draft');
  await run(()=>{const field=document.querySelector('#test-name');field.value='';field.dispatchEvent(new Event('input',{bubbles:true}));});
  assert.equal(await run(()=>document.querySelector('#one [type=submit]').disabled),true);
  assert.equal(await run(()=>document.querySelector('#test-name').getAttribute('aria-invalid')),'true');
  assert.equal(await run(()=>document.querySelector('#name-error').hidden),false);
  await run(()=>document.querySelector('#one [data-cancel]').click());
  assert.equal(await run(()=>document.querySelector('#test-name').value),'A&B +% "雪"');
  await run(()=>{const field=document.querySelector('#test-password');field.value='DRAFT-SECRET';field.dispatchEvent(new Event('input',{bubbles:true}));document.querySelector('#two').requestSubmit();});
  await page.wait("window.__formTest.second.state==='dirty' && !window.__formTest.second.busy");
  assert.equal(await run(()=>document.querySelector('#test-password').value),'DRAFT-SECRET');
  assert.equal(await run(()=>JSON.stringify({...localStorage}).includes('DRAFT-SECRET')),false);
 });
 await check('Overlapping notices cannot dismiss a newer notice',async()=>{
  await run(()=>{const notice=window.__formTest.notice;const old=notice.show('Old notice',{duration:15});notice.show('New notice',{duration:120});notice.clear(old);});
  await sleep(35);assert.equal(await run(()=>document.querySelector('#notice-test').textContent),'New notice');
  await sleep(130);assert.equal(await run(()=>document.querySelector('#notice-test').hidden),true);
  await run(()=>{window.__formTest.first.destroy();window.__formTest.second.destroy();});
  assert.equal(await run(()=>document.querySelector('#test-password').value),'');
  await run(()=>document.querySelector('#form-test').remove());
 });
 await check('Serial errors show causes and fresh deltas consistently on overview, Diagnostics, Serial and export',async()=>{
  const value=await(await fetch(base+'/api/v1/status')).json();value.serial.boot_id=700;value.serial.errors.uart_frames=2;value.serial.errors.extra_secret='TEST-SECRET';value.system.uptime_ms=500000;
  await run(()=>location.hash='#/dashboard');await scenario('normal','/api/v1/status',value);await page.wait("!document.querySelector('#refresh').disabled");await refresh();
  await page.wait("document.querySelector('[data-card=serial] [data-part=badge]').textContent==='Serial errors recorded'");
  value.system.uptime_ms+=1000;await scenario('normal','/api/v1/status',value);await page.wait("!document.querySelector('#refresh').disabled");await refresh();await page.wait("document.querySelector('[data-card=serial] [data-part=badge]').textContent==='Earlier serial errors'");
  value.serial.errors.uart_frames++;value.system.uptime_ms+=1000;await scenario('normal','/api/v1/status',value);await page.wait("!document.querySelector('#refresh').disabled");await refresh();await page.wait("document.querySelector('[data-card=serial] [data-part=badge]').textContent==='New serial errors'");
  assert((await run(()=>document.querySelector('[data-card=serial] [data-part=detail]').textContent)).includes('UART framing errors: +1'));
  await run(()=>location.hash='#/diagnostics');await page.wait("document.querySelector('#diagnostics-serial-health')?.textContent.includes('UART framing errors: 3')");
  const data=await page.evaluate(`(async()=>{const m=await import('/__test/management.js');return m.redactedDiagnostics(${JSON.stringify(value)},null);})()`);assert.equal(data.serial.errors.uart_frames,3);assert.equal('extra_secret' in data.serial.errors,false);assert.equal(data.serial.error_observation.interval_ms,null);
  const exportCurrent=()=>run(async()=>{const create=URL.createObjectURL,click=HTMLAnchorElement.prototype.click;let blob;URL.createObjectURL=value=>{blob=value;return create.call(URL,value);};HTMLAnchorElement.prototype.click=()=>{};try{document.querySelector('#diagnostics-export').click();return JSON.parse(await blob.text());}finally{URL.createObjectURL=create;HTMLAnchorElement.prototype.click=click;}});
  const saved=await exportCurrent();assert.equal(saved.serial.errors.uart_frames,3);assert.equal(saved.serial.error_observation.changes.uart_frames,0);assert.equal(saved.serial.error_observation.stale,false);assert(!JSON.stringify(saved).includes('TEST-SECRET'));
  await scenario('http-error');await page.wait("!document.querySelector('#refresh').disabled");await refresh();await page.wait("document.querySelector('#diagnostics-serial-health').textContent.includes('out of date')");assert.equal((await exportCurrent()).serial.error_observation.stale,true);
  await scenario('normal','/api/v1/status',value);await page.wait("!document.querySelector('#refresh').disabled");await refresh();await page.wait("document.querySelector('#diagnostics-serial-health').textContent.includes('Waiting for the next reading')");
  await run(()=>location.hash='#/serial');await page.wait("document.querySelector('#serial-health')?.textContent.includes('UART framing errors: 3') && !document.querySelector('#serial-health').hidden");
  await scenario('normal');await run(()=>location.hash='#/dashboard');await page.wait("document.querySelector('[data-card=serial] [data-part=badge]')?.textContent==='TCP listener ready'");
 });
 await testManagementBrowser({page,run,check,viewport,noOverflow});
 await testUpdatesBrowser({page,run,check,viewport,noOverflow});
 await testNetworkBrowser({page,run,check,viewport,noOverflow});
 await testSerialBrowser({page,run,check,viewport,noOverflow});
 await testBrandingBrowser({page,run,check,viewport,noOverflow});
 }
 assert.deepEqual(exceptions,[]);
 await writeFile(join(output,'result.json'),JSON.stringify({pass:true,checks:results},null,2)+'\n');
} catch(error) {
 await page.screenshot('failure.png').catch(()=>{});
 await writeFile(join(output,'failure-dom.html'),await page.evaluate('document.documentElement.outerHTML').catch(()=>''));
 throw error;
} finally { await page.close(); }
