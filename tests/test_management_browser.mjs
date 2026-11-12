import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
export async function testManagementBrowser({page,run,check,viewport,noOverflow}){
 const fixtures=JSON.parse(await readFile(new URL('./fixtures/api-v1.json',import.meta.url)));
 const mock=()=>{
  const real=window.fetch.bind(window);window.__managementOriginalFetch=real;
  window.fetch=async(url,options={})=>{
   const path=new URL(url,location.href).pathname,state=window.__managementTest;
   const response=(value,status=200)=>new Response(JSON.stringify(value),{status,headers:{'Content-Type':'application/json'}});
   if(path==='/api/v1/diagnostics/uart-log'){state.mode ||= {schema:1,mode:'off',uart_enabled:false};if(options.method==='POST'){state.mode.mode=new URLSearchParams(options.body).get('mode');state.mode.uart_enabled=state.mode.mode!=='off';}return response(state.mode);}
   const group=path.split('/')[3],root='/api/v1/'+group;
   if(!['settings','integrations'].includes(group))return real(url,options);
   const config=state[root];if(options.method!=='POST')return response(config);
   const values=Object.fromEntries(new URLSearchParams(options.body));state.posts.push({path,values});
   if(state.reject)return response({error:{code:'SAVE_FAILED'}},503);
   const key=path.split('/')[4];config.sequence++;
   if(key==='description')config.identity.description=values.description;
   else if(key==='mqtt')for(const [k,v] of Object.entries(values)){
    if(k==='password'||k==='password_action')continue;
    config.mqtt[k]=['port','timeout','keepalive'].includes(k)?Number(v):['enabled','clean_session'].includes(k)?v==='true':v;
   }
   return response(config);
  };
  location.hash='#/settings';
 };
 const source=`window.__managementTest=${JSON.stringify({...Object.fromEntries(['settings','integrations'].map(k=>['/api/v1/'+k,fixtures['/api/v1/'+k]])),posts:[],reject:false})};(${mock.toString()})();`;
 const script=await page.send('Page.addScriptToEvaluateOnNewDocument',{source});
 await page.send('Page.reload',{ignoreCache:true});
 await page.wait("document.querySelector('#management-description')?.dataset.state==='clean'");
 await check('Settings: grouped save/readback, GPIO conflicts and responsive view',async()=>{
  await run(()=>{const f=document.querySelector('#management-description');f.elements.description.value='Updated recovery console';f.dispatchEvent(new Event('input'));window.__managementTest['/api/v1/settings'].identity.hostname='network-changed';f.requestSubmit();});
  await page.wait("document.querySelector('#management-description')?.dataset.state==='saved'");
  assert.equal(await run(()=>window.__managementTest.posts.length),1);
  assert.deepEqual(await run(()=>window.__managementTest.posts[0].values),{description:'Updated recovery console'});
  assert.equal(await run(()=>window.__managementTest['/api/v1/settings'].identity.hostname),'network-changed');
  assert.equal(await run(()=>document.querySelector('#management-description-hostname')),null);
  assert.equal(await run(()=>document.querySelector('#management-discovery')),null);
  assert.equal(await run(()=>document.querySelector('#management-description').parentElement.querySelector('a')),null);
  await run(()=>{const f=document.querySelector('#management-pins');f.elements.reset.value='1';f.dispatchEvent(new Event('input'));});
  assert.equal(await run(()=>document.querySelector('#management-pins [type=submit]').disabled),true);
  assert.equal(await run(()=>document.querySelector('#management-pins-reset').getAttribute('aria-invalid')),'true');
  await run(()=>document.querySelector('#management-pins [data-cancel]').click());
  for(const width of [360,1440]){await viewport(width);await noOverflow();}
  await page.screenshot('settings.png');
 });
 await check('Integrations: clear feature groups, collapsed setup, and separate authentication/options',async()=>{
  await run(()=>location.hash='#/integrations');await page.wait("document.querySelector('#management-mqtt')?.dataset.state==='clean'");
  assert.deepEqual(await run(()=>[...document.querySelectorAll('#management-forms h2')].map(n=>n.textContent)),['MQTT broker','Publish device status','Microcontroller integration']);
  assert.equal(await run(()=>document.querySelector('#integration-mqtt-settings').open),false);
  assert.equal(await run(()=>document.querySelector('#integration-reporting-settings').open),false);
  assert.equal(await run(()=>document.querySelector('#integration-slip-settings').open),false);
  assert.equal(await run(()=>document.querySelector('#integration-slip-settings summary').textContent),'Advanced: serial command protocol');
  assert((await run(()=>document.querySelector('#integration-slip').textContent)).includes('Leave this off for ordinary serial devices.'));

  assert((await run(()=>document.querySelector('.management-intro').textContent)).includes('normal serial console'));
  assert.equal(await run(()=>document.querySelector('#integration-reporting-settings').textContent.includes('Example status message')),true);
  assert.equal(await run(()=>document.querySelector('#management-state').textContent),'Broker connection: Off');
  const count=await run(()=>window.__managementTest.posts.length);
  await run(()=>{document.querySelector('#integration-mqtt-settings').open=true;});
  assert.equal(await run(()=>window.__managementTest.posts.length),count);
  const panels=await run(()=>[...document.querySelectorAll('#management-mqtt .management-options summary')].map(n=>n.textContent));assert.deepEqual(panels,['Authentication','Connection options']);
  await run(()=>{const f=document.querySelector('#management-mqtt');f.elements.timeout.value='0';f.dispatchEvent(new Event('input'));});
  assert.equal(await run(()=>document.querySelector('#management-mqtt-timeout').closest('details').open),true);
  assert.equal(await run(()=>document.querySelector('#management-mqtt-timeout').getAttribute('aria-invalid')),'true');
  await run(()=>document.querySelector('#management-mqtt [data-cancel]').click());
 });
 await check('Integrations: secret drafts survive failed save, success clears them',async()=>{
  await run(()=>location.hash='#/integrations');
  await page.wait("document.querySelector('#management-mqtt')?.dataset.state==='clean'");
  await run(()=>{document.querySelector('#integration-mqtt-settings').open=true;document.querySelector('#management-mqtt-password').closest('details').open=true;});
  assert.equal(await run(()=>document.querySelector('#management-mqtt-password').value),'');
  await run(()=>{window.__managementTest.reject=true;const f=document.querySelector('#management-mqtt');f.elements.password_action.value='replace';f.elements.password.value='DUMMY-TEST-SECRET';f.dispatchEvent(new Event('input'));f.requestSubmit();});
  await page.wait("document.querySelector('#management-mqtt')?.dataset.state==='dirty' && document.querySelector('#management-mqtt [role=status]').textContent.includes('couldn’t confirm the save')");
  assert.equal(await run(()=>document.querySelector('#management-mqtt-password').value),'DUMMY-TEST-SECRET');
  assert.equal(await run(()=>window.__managementTest.posts.length),2);
  await run(()=>{window.__managementTest.reject=false;document.querySelector('#management-mqtt').requestSubmit();});
  await page.wait("document.querySelector('#management-mqtt')?.dataset.state==='saved'");
  assert.equal(await run(()=>document.querySelector('#management-mqtt-password').value),'');
  assert.equal(await run(()=>window.__managementTest.posts.length),3);
  await page.screenshot('integrations.png');
 });
 await check('Integrations: unavailable MQTT leaves MCU setup usable without empty broker forms',async()=>{
  await run(()=>{window.__managementSavedMqtt=window.__managementTest['/api/v1/integrations'].mqtt;window.__managementTest['/api/v1/integrations'].mqtt=null;document.querySelector('#management-reload').click();});
  await page.wait("!document.querySelector('#management-mqtt') && document.querySelector('#management-slip')?.dataset.state==='clean'");
  assert((await run(()=>document.querySelector('#management-forms').textContent)).includes('aren’t included'));
  assert.equal(await run(()=>document.querySelector('#management-reporting')),null);
  await run(()=>{window.__managementTest['/api/v1/integrations'].mqtt=window.__managementSavedMqtt;delete window.__managementSavedMqtt;document.querySelector('#management-reload').click();});
  await page.wait("document.querySelector('#management-mqtt')?.dataset.state==='clean'");
 });
 await check('Settings and Integrations keep the divider below the reload toolbar at enlarged text',async()=>{
  for(const route of ['settings','integrations'])for(const [width,scale] of [[360,'100%'],[1440,'100%'],[360,'200%']]){
   await viewport(width);await page.evaluate(`document.documentElement.style.fontSize=${JSON.stringify(scale)};location.hash=${JSON.stringify('#/'+route)}`);
   await page.wait(route==='settings'?"document.querySelector('#management-description')?.dataset.state==='clean'":"document.querySelector('#management-mqtt')?.dataset.state==='clean'");
   const gap=await run(()=>document.querySelector('#management-forms>section').getBoundingClientRect().top-document.querySelector('#management-reload').getBoundingClientRect().bottom);assert(gap>=20,'Reload button/divider gap: '+gap);await noOverflow();
  }
  await run(()=>document.documentElement.style.fontSize='');await viewport(1440);
 });
 await check('Diagnostics: bounded log deduplication and allowlisted export',async()=>{
  await run(()=>location.hash='#/diagnostics');
  await page.wait("document.querySelector('#diagnostics-log')?.textContent.includes('Device ready.')");
  for(const [width,scale] of [[360,'100%'],[768,'100%'],[1440,'100%'],[360,'200%']]){
   await viewport(width);await page.evaluate(`document.documentElement.style.fontSize=${JSON.stringify(scale)}`);await noOverflow();
   const gaps=await run(()=>{const b=document.querySelector('#diagnostics-export').getBoundingClientRect(),h=document.querySelector('#diagnostics-log-heading').getBoundingClientRect(),a=document.querySelector('#diagnostics-log-actions').getBoundingClientRect(),log=document.querySelector('#diagnostics-log').getBoundingClientRect();return {heading:h.top-b.bottom,log:log.top-a.bottom,buttons:document.querySelector('#diagnostics-log-actions').children.length};});
   assert(gaps.heading>=20,'Diagnostics heading/export gap: '+JSON.stringify(gaps));assert(gaps.log>=20,'Log controls/output gap: '+JSON.stringify(gaps));assert.equal(gaps.buttons,3);
  }
  await viewport(1440);await run(()=>document.documentElement.style.fontSize='');
  const before=await run(()=>document.querySelector('#diagnostics-log').textContent);
  await run(()=>document.querySelector('#refresh').click());
  await page.wait("!document.querySelector('#refresh').disabled");
  assert.equal(await run(()=>document.querySelector('#diagnostics-log').textContent),before);
  const data=await page.evaluate(`(async()=>{const m=await import('/__test/management.js');return m.redactedDiagnostics(${JSON.stringify(fixtures['/api/v1/status'])},${JSON.stringify(fixtures['/api/v1/capabilities'])});})()`);
  assert.equal('hostname' in data,false);assert.equal('ssid' in data.wifi,false);assert.equal('description' in data,false);
  await page.wait("document.querySelector('[data-mode=off]')?.getAttribute('aria-pressed')==='true'");
  await run(()=>{window.confirm=()=>true;document.querySelector('[data-mode=on1]').click();});
  await page.wait("document.querySelector('[data-mode=on1]')?.getAttribute('aria-pressed')==='true'");
  await run(()=>document.querySelector('#diagnostics-export').scrollIntoView({block:'center'}));await page.screenshot('diagnostics.png');
 });
 await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:script.identifier});
 await page.send('Page.reload',{ignoreCache:true});
 await page.wait("document.body?.dataset.ready==='true'");
}
