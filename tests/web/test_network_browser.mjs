import {sleep} from '../support/headless_browser.mjs';
import assert from 'node:assert/strict';
import {readFile} from 'node:fs/promises';
export async function testNetworkBrowser({page,run,check,viewport,noOverflow}){
 const fixtures=JSON.parse(await readFile(new URL('../support/fixtures/api-v1.json',import.meta.url)));
 const mock=()=>{
  const real=window.fetch.bind(window);window.__networkOriginalFetch=real;
  window.fetch=async (url,options={})=>{
   const path=new URL(url,location.href).pathname,n=window.__networkTest;
   const response=(value,status=200)=>new Response(JSON.stringify(value),{status,headers:{'Content-Type':'application/json'}});
   if(path==='/api/v1/status')return response(n.status);
   if(!path.startsWith('/api/v1/network'))return real(url,options);
   if(options.method==='POST'){
    if(options.headers['X-Serial2WiFi-Request']!=='1')return response({},415);
    const v=Object.fromEntries(new URLSearchParams(options.body));n.posts.push({path,v,url:String(url)});
    if(path.endsWith('/scan')){n.scanResponse={schema:1,state:'complete',total:n.scanEmpty?0:3,retained:n.scanEmpty?0:3,page:0,truncated:false,networks:n.scanEmpty?[]:[
      {ssid:'Same network',bssid:'02:11:22:33:44:55',rssi_dbm:-35,channel:1,security:'wpa2'},
      {ssid:'Same network',bssid:'02:11:22:33:44:66',rssi_dbm:-50,channel:6,security:'wpa2'},
      {ssid:'Legacy WEP',bssid:'02:11:22:33:44:77',rssi_dbm:-70,channel:1,security:'wep'}]};return response({...n.scanResponse,state:n.scanFailures?'scanning':'complete'},202);}
    if(n.mode==='rejected')return response({error:{code:'INVALID_NETWORK'}},422);
    n.config.operation={id:100+n.posts.length,state:'applied',error:null};
    if(path.endsWith('/station')){
     n.config.station={ssid:v.ssid,password_configured:v.security!=='open',bssid:v.bssid||null};n.config.mode='station_ap';
     n.config.ip={dhcp:v.dhcp==='true',...Object.fromEntries(['address','mask','gateway','dns1','dns2'].map(k=>[k,v[k]]))};
     n.status.wifi={...n.status.wifi,mode:'station_ap',state:['wrong-password','dhcp-timeout'].includes(n.mode)?'failed':'connected',reason:n.mode==='wrong-password'?'wrong_password':n.mode==='dhcp-timeout'?'dhcp_timeout':null,ssid:v.ssid,ip:v.dhcp==='true'?'10.1.1.3':v.address};
    }else if(path.endsWith('/ap')){
     n.config.ap={ssid:v.ssid,password_configured:v.security!=='open',security:v.security,channel:Number(v.channel),hidden:v.hidden==='true',address:v.address};n.config.mode='ap';
     n.status.wifi={...n.status.wifi,mode:'ap',state:'ready',reason:null,ssid:v.ssid,ip:v.address,ap_ip:v.address};
    }else n.config.identity={hostname:v.hostname,mdns:v.mdns==='true',mdns_name:v.mdns_name};
    if(n.mode==='channel-change')n.offline=2;
    return response({id:n.config.operation.id,state:'accepted',error:null},202);
   }
   if(path.endsWith('/scan/0')){if(n.scanFailures>0){n.scanFailures--;throw new TypeError('Synthetic scan pause');}return response(n.scanResponse);}
   if(n.offline>0){n.offline--;throw new TypeError('Synthetic disconnect');}
   return response(n.config);
  };
  location.hash='#/network';
 };
 const script=await page.send('Page.addScriptToEvaluateOnNewDocument',{source:`window.__networkTest={config:${JSON.stringify(fixtures['/api/v1/network'])},status:${JSON.stringify(fixtures['/api/v1/status'])},posts:[],mode:'normal',scanEmpty:true,offline:0};(${mock.toString()})();`});
 await page.send('Page.reload',{ignoreCache:true});
 await page.wait("document.querySelectorAll('#network-forms form[data-state=clean]').length===3");

 await check('Network setup shows one mode, preserves drafts and never applies a tab selection',async()=>{
  assert.equal(await run(()=>document.querySelector('#network-current-mode').textContent),'Standalone Wi-Fi');
  assert.equal(await run(()=>document.querySelector('#network-ap-panel').hidden),false);
  assert.equal(await run(()=>[...document.querySelectorAll('.network-overview dt')].find(n=>n.textContent==='Device address').nextElementSibling.textContent),'192.168.4.1');
  await run(()=>{window.__networkTest.status.wifi.ip=null;window.__networkTest.status.wifi.ap_ip=null;document.querySelector('#refresh').click();});
  await page.wait("document.querySelector('#device-address').textContent==='Address unavailable'");
  assert.equal(await run(()=>[...document.querySelectorAll('.network-overview dt')].find(n=>n.textContent==='Device address').nextElementSibling.textContent),'Address unavailable');
  await run(()=>{window.__networkTest.status.wifi.ip='192.168.4.1';window.__networkTest.status.wifi.ap_ip='192.168.4.1';document.querySelector('#refresh').click();});
  await page.wait("document.querySelector('#device-address').textContent==='192.168.4.1'");

  assert.equal(await run(()=>document.querySelector('#network-station-panel').hidden),true);
  assert.equal(await run(()=>[...document.querySelectorAll('#view button')].some(b=>b.textContent==='Open AP-only setup')),false);
  assert.equal(await run(()=>document.querySelector('#network-identity-settings').open),false);
  await viewport(1440);await page.screenshot('network-redesign-ap-1440.png');
  await run(()=>document.querySelector('#network-tab-station').click());
  assert.equal(await run(()=>document.querySelector('#network-station-panel').hidden),false);
  assert.equal(await run(()=>document.querySelector('#network-ap-panel').hidden),true);
  assert.equal(await run(()=>document.querySelector('#network-station-advanced').open),false);
  await run(()=>{const f=document.querySelector('#network-station');f.elements.ssid.value='draft-network';f.elements.password_action.value='replace';f.elements.password.value='DUMMY-TAB-DRAFT';f.dispatchEvent(new Event('input'));document.querySelector('#network-tab-ap').click();document.querySelector('#network-tab-station').click();});
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'DUMMY-TAB-DRAFT');
  assert.equal(await run(()=>window.__networkTest.config.mode),'ap');assert.equal(await run(()=>window.__networkTest.posts.length),0);
  await run(()=>document.querySelector('#network-station [data-cancel]').click());
  await run(()=>document.querySelector('#network-tab-station').focus());await page.key('ArrowRight');
  assert.equal(await run(()=>document.activeElement.id),'network-tab-ap');assert.equal(await run(()=>document.querySelector('#network-tab-ap').getAttribute('aria-selected')),'true');
  await page.key('ArrowLeft');assert.equal(await run(()=>document.activeElement.id),'network-tab-station');
  await page.screenshot('network-redesign-station-1440.png');
 });

 await check('Network password controls reveal only the fields needed for the chosen action',async()=>{
  await run(()=>{const input=document.querySelector('#network-station-ssid');input.value='manual-network';input.dispatchEvent(new Event('input',{bubbles:true}));});
  assert.equal(await run(()=>document.querySelector('#network-station-password_action').value),'replace');
  assert.equal(await run(()=>document.querySelector('#network-station-password').closest('.form-field').hidden),false);
  await run(()=>{const f=document.querySelector('#network-station');f.elements.password.value='DUMMY-NEW-PASS';f.elements.password_action.value='clear';f.elements.password_action.dispatchEvent(new Event('change',{bubbles:true}));});
  assert.equal(await run(()=>document.querySelector('#network-station-security').value),'open');
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'');
  assert.equal(await run(()=>document.querySelector('#network-station-password').closest('.form-field').hidden),true);
  await run(()=>document.querySelector('#network-station [data-cancel]').click());
  assert.equal(await run(()=>window.__networkTest.posts.length),0);
 });
 await check('Network forms load redacted drafts and retain edits across status polls',async()=>{
  assert.equal(await run(()=>document.querySelector('#network-ap-password').value),'');
  await run(()=>{
   const form=document.querySelector('#network-station');form.elements.ssid.value='new&+%雪';
   form.elements.password_action.value='replace';form.elements.password.value='DUMMY-NET-PASSWORD';form.dispatchEvent(new Event('input'));
  });
  await run(()=>document.querySelector('#refresh').click());
  await page.wait("!document.querySelector('#refresh').disabled");
  assert.equal(await run(()=>document.querySelector('#network-station-ssid').value),'new&+%雪');
  assert.equal(await run(()=>window.__networkTest.posts.length),0);
  await run(()=>document.querySelector('#network-station button[aria-pressed]').click());
  assert.equal(await run(()=>document.querySelector('#network-station-password').type),'text');
  await run(()=>document.querySelector('#network-station [data-cancel]').click());
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'');
 });
 await check('Network scans expose empty state, duplicate BSSIDs and explicit selection',async()=>{
  await run(()=>document.querySelector('[data-scan]').click());
  await page.wait("document.querySelector('[data-scan-results]').textContent.includes('No networks found')");
  assert.equal(await run(()=>document.querySelector('#network-station-ssid').value),'demo-router');
  await run(()=>{window.__networkTest.scanEmpty=false;document.querySelector('[data-scan]').click();});
  await page.wait("document.querySelectorAll('.scan-list li').length===3");
  assert.equal(await run(()=>document.querySelectorAll('.scan-list button:disabled').length),1);
  await run(()=>document.querySelectorAll('.scan-list button')[1].click());
  assert.equal(await run(()=>document.querySelector('#network-station-bssid').value),'02:11:22:33:44:66');
  await run(()=>document.querySelector('[data-scan]').click());await page.wait("!document.querySelector('[data-scan]').disabled");
  assert.equal(await run(()=>document.querySelector('#network-station-bssid').value),'02:11:22:33:44:66');
 });
 await check('Scan tolerates a bounded radio pause without repeating its POST',async()=>{
  const before=await run(()=>window.__networkTest.posts.filter(p=>p.path.endsWith('/scan')).length);
  await run(()=>{window.__networkTest.scanFailures=2;document.querySelector('[data-scan]').click();});
  await page.wait("document.querySelector('[data-scan-results]').textContent.includes('Still waiting for the scan')");
  await page.wait("!document.querySelector('[data-scan]').disabled && document.querySelectorAll('.scan-list li').length===3",10000);
  assert.equal(await run(()=>window.__networkTest.posts.filter(p=>p.path.endsWith('/scan')).length),before+1);
  assert.equal(await run(()=>document.querySelector('#network-station-bssid').value),'02:11:22:33:44:66');
 });
 await check('Network Save verifies receipt and live connection; secrets stay in bodies',async()=>{
  await run(()=>{
   const f=document.querySelector('#network-station');f.elements.ssid.value='new&+%雪';f.elements.bssid.value='';
   f.elements.password.value='DUMMY-NET-PASSWORD';f.dispatchEvent(new Event('input'));f.requestSubmit();f.requestSubmit();
  });
  await page.wait("document.querySelector('#network-station').dataset.state==='saved'",15000);
  assert.equal(await run(()=>document.querySelector('#network-stage').dataset.state),'connected');
  assert.equal(await run(()=>window.__networkTest.posts.filter(p=>p.path.endsWith('/station')).length),1);
  assert.equal(await run(()=>window.__networkTest.posts.at(-1).v.password),'DUMMY-NET-PASSWORD');
  assert.equal(await run(()=>window.__networkTest.posts.every(p=>!p.url.includes('?'))),true);
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'');
  assert.equal(await run(()=>JSON.stringify({...localStorage}).includes('DUMMY-NET-PASSWORD')),false);
 });
 await check('Wrong password preserves draft and reports recovery, never connected',async()=>{
  await run(()=>{
   window.__networkTest.mode='wrong-password';const f=document.querySelector('#network-station');
   f.elements.ssid.value='wrong-password-router';f.elements.password_action.value='replace';f.elements.password.value='DUMMY-RETAINED';
   f.dispatchEvent(new Event('input'));f.requestSubmit();
  });
  await page.wait("document.querySelector('#network-stage').dataset.state==='recovery' && document.querySelector('#network-station').dataset.state==='dirty'",15000);
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'DUMMY-RETAINED');
  assert((await run(()=>document.querySelector('#network-stage').textContent)).includes('rejected the password'));
  await run(()=>document.querySelector('#network-station [data-cancel]').click());
  assert.equal(await run(()=>document.querySelector('#network-station-ssid').value),'wrong-password-router');
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'');
 });
 await check('DHCP timeout identifies the failure and preserves the station draft',async()=>{
  await run(()=>{
   window.__networkTest.mode='dhcp-timeout';const f=document.querySelector('#network-station');
   f.elements.ssid.value='dhcp-timeout-router';f.elements.password_action.value='replace';f.elements.password.value='DUMMY-DHCP-DRAFT';
   f.dispatchEvent(new Event('input'));f.requestSubmit();
  });
  await page.wait("document.querySelector('#network-stage').dataset.state==='recovery' && document.querySelector('#network-station').dataset.state==='dirty'",15000);
  assert((await run(()=>document.querySelector('#network-stage').textContent)).includes('DHCP address'));
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'DUMMY-DHCP-DRAFT');
  await run(()=>document.querySelector('#network-station [data-cancel]').click());
  assert.equal(await run(()=>document.querySelector('#network-station-password').value),'');
 });
 await check('Static address validation blocks invalid writes and verifies the resulting IP',async()=>{
  const before=await run(()=>window.__networkTest.posts.length);
  await run(()=>{
   window.__networkTest.mode='normal';const f=document.querySelector('#network-station');
   f.elements.ssid.value='static-router';f.elements.password_action.value='replace';f.elements.password.value='DUMMY-VALID-PASS';
   f.elements.dhcp.checked=false;f.elements.address.value='256.1.1.3';f.elements.mask.value='255.255.255.0';f.elements.gateway.value='10.1.1.1';
   f.elements.dns1.value='1.1.1.1';f.elements.dns2.value='0.0.0.0';f.dispatchEvent(new Event('input'));f.requestSubmit();
  });
  assert.equal(await run(()=>window.__networkTest.posts.length),before);
  assert.equal(await run(()=>document.querySelector('#network-station-ip-address').getAttribute('aria-invalid')),'true');
  await run(()=>{
   const f=document.querySelector('#network-station');f.elements.address.value='10.1.1.3';f.dispatchEvent(new Event('input'));f.requestSubmit();
  });
  await page.wait("document.querySelector('#network-station').dataset.state==='saved'",15000);
  assert.equal(await run(()=>document.querySelector('#network-stage').dataset.state),'connected');
 });
 await check('AP channel disconnect retains reconnect guidance until verified',async()=>{
  await run(()=>{document.querySelector('#network-tab-ap').click();document.querySelector('#network-ap-advanced').open=true;});
  await run(()=>{
   window.__networkTest.mode='channel-change';const f=document.querySelector('#network-ap');
   f.elements.ssid.value='new-recovery';f.elements.channel.value='6';f.dispatchEvent(new Event('input'));f.requestSubmit();
  });
  await page.wait("document.querySelector('#network-stage').dataset.state==='awaiting_reconnect'");
  assert((await run(()=>document.querySelector('.network-recovery').textContent)).includes('new-recovery'));
  await page.wait("document.querySelector('#network-ap').dataset.state==='saved'",20000);
  assert.equal(await run(()=>window.__networkTest.config.mode),'ap');
 });

 await check('Standalone address validates before saving and keeps the new URL through reconnection',async()=>{
  const before=await run(()=>window.__networkTest.posts.length);
  await run(()=>{const f=document.querySelector('#network-ap');f.elements.address.value='10.30.7.255';f.dispatchEvent(new Event('input'));f.requestSubmit();});
  assert.equal(await run(()=>window.__networkTest.posts.length),before);
  assert.equal(await run(()=>document.querySelector('#network-ap').elements.address.getAttribute('aria-invalid')),'true');
  await run(()=>{const f=document.querySelector('#network-ap');f.elements.address.value='10.30.7.254';f.dispatchEvent(new Event('input'));});
  assert((await run(()=>document.querySelector('#network-ap').textContent)).includes('http://10.30.7.254/'));
  await viewport(360);await noOverflow();await page.screenshot('network-ap-address-360.png');
  await run(()=>{window.__networkTest.mode='channel-change';document.querySelector('#network-ap').requestSubmit();});
  await page.wait("document.querySelector('#network-stage').dataset.state==='awaiting_reconnect'");
  assert.equal(await run(()=>document.querySelector('.network-recovery a').hostname),'10.30.7.254');
  assert((await run(()=>document.querySelector('.network-recovery').textContent)).includes('10.30.7.254'));
  await page.wait("document.querySelector('#network-ap').dataset.state==='saved'",20000);
  assert.equal(await run(()=>window.__networkTest.config.ap.address),'10.30.7.254');
  assert.equal(await run(()=>window.__networkTest.posts.at(-1).v.address),'10.30.7.254');
  await viewport(1440);await page.screenshot('network-ap-address-1440.png');
 });
 await check('Standalone password removal updates the saved-password choice after readback',async()=>{
  await run(()=>{window.__networkTest.mode='normal';const f=document.querySelector('#network-ap');f.elements.password_action.value='clear';f.elements.password_action.dispatchEvent(new Event('change',{bubbles:true}));f.requestSubmit();});
  await page.wait("document.querySelector('#network-ap').dataset.state==='saved'",15000);
  assert.equal(await run(()=>window.__networkTest.config.ap.password_configured),false);
  assert.equal(await run(()=>document.querySelector('#network-ap-password_action').value),'keep');
  assert.equal(await run(()=>document.querySelector('#network-ap-password_action option[value=keep]').textContent),'No password (current setting)');
 });
 await check('Network dirty navigation guard',async()=>{
  await run(()=>{const f=document.querySelector('#network-ap');f.elements.password_action.value='replace';f.elements.password.value='DUMMY-LEAVE';f.dispatchEvent(new Event('input'));window.__oldConfirm=window.confirm;window.confirm=()=>false;location.hash='#/dashboard';});
  await page.wait("location.hash==='#/network'");
  assert.equal(await run(()=>document.querySelector('#network-ap-password').value),'DUMMY-LEAVE');
  await run(()=>{window.confirm=()=>true;location.hash='#/dashboard';});
  await page.wait("document.querySelector('[data-card=wifi]')!==null");
  await run(()=>{window.confirm=window.__oldConfirm;window.fetch=window.__networkOriginalFetch;});
 });
 await page.send('Page.removeScriptToEvaluateOnNewDocument',{identifier:script.identifier});
 await page.send('Page.reload',{ignoreCache:true});
 await page.wait("document.body?.dataset.ready==='true'");
}

export async function testNetworkRecovery({page,run,check,fresh,hold,release,count,stage}) {
 await fresh('network');await page.wait("document.querySelector('#network-ap')?.dataset.state==='clean'");
 await check('Pending scan cannot overwrite a save; invalid receipt ends in unverified',async()=>{
  const key='POST /api/v1/network/scan';await hold(key);
  await run(()=>{document.querySelector('#network-tab-station').click();document.querySelector('[data-scan]').click();});await page.wait("!!t.pending['POST /api/v1/network/scan']");
  await run(()=>{document.querySelector('#network-tab-ap').click();document.querySelector('#network-ap').requestSubmit();});await release(key);
  await page.wait("document.querySelector('#network-stage')?.dataset.state==='unverified'");
  assert.equal(await run(()=>document.querySelector('#network-stage').dataset.state),'unverified');
  assert.equal(await count('POST /api/v1/network/ap'),1);
  assert.equal(await run(()=>document.querySelector('[data-scan]').disabled),false);
 });
}

export async function testNetworkReload({page,run,check,base}) {
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
}
